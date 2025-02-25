/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_sending.h"

#include "api/api_text_entities.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_channel.h" // ChannelData::addsSignature.
#include "data/data_user.h" // UserData::name
#include "data/data_session.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_changes.h"
#include "data/stickers/data_stickers.h"
#include "history/history.h"
#include "history/history_message.h" // NewMessageFlags.
#include "chat_helpers/message_field.h" // ConvertTextTagsToEntities.
#include "chat_helpers/stickers_dice_pack.h" // DicePacks::kDiceString.
#include "ui/text/text_entity.h" // TextWithEntities.
#include "ui/item_text_options.h" // Ui::ItemTextOptions.
#include "main/main_session.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "storage/localimageloader.h"
#include "storage/file_upload.h"
#include "mainwidget.h"
#include "apiwrap.h"

namespace Api {
namespace {

void InnerFillMessagePostFlags(
		const Api::SendOptions &options,
		not_null<PeerData*> peer,
		MessageFlags &flags) {
	const auto anonymousPost = peer->amAnonymous();
	if (!anonymousPost) {
		flags |= MessageFlag::HasFromId;
		return;
	} else if (peer->asMegagroup()) {
		return;
	}
	flags |= MessageFlag::Post;
	// Don't display views and author of a new post when it's scheduled.
	if (options.scheduled) {
		return;
	}
	flags |= MessageFlag::HasViews;
	if (peer->asChannel()->addsSignature()) {
		flags |= MessageFlag::HasPostAuthor;
	}
}

template <typename MediaData>
void SendExistingMedia(
		Api::MessageToSend &&message,
		not_null<MediaData*> media,
		Fn<MTPInputMedia()> inputMedia,
		Data::FileOrigin origin) {
	const auto history = message.action.history;
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto api = &session->api();

	message.action.clearDraft = false;
	message.action.generateLocal = true;
	api->sendAction(message.action);

	const auto newId = FullMsgId(
		peerToChannel(peer->id),
		session->data().nextLocalMessageId());
	const auto randomId = base::RandomValue<uint64>();

	auto flags = NewMessageFlags(peer);
	auto sendFlags = MTPmessages_SendMedia::Flags(0);
	if (message.action.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
		sendFlags |= MTPmessages_SendMedia::Flag::f_reply_to_msg_id;
	}
	const auto anonymousPost = peer->amAnonymous();
	const auto silentPost = ShouldSendSilent(peer, message.action.options);
	InnerFillMessagePostFlags(message.action.options, peer, flags);
	if (silentPost) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_silent;
	}
	auto messageFromId = anonymousPost ? 0 : session->userPeerId();
	auto messagePostAuthor = peer->isBroadcast() ? session->user()->name : QString();

	auto caption = TextWithEntities{
		message.textWithTags.text,
		TextUtilities::ConvertTextTagsToEntities(message.textWithTags.tags)
	};
	TextUtilities::Trim(caption);
	auto sentEntities = EntitiesToMTP(
		session,
		caption.entities,
		ConvertOption::SkipLocal);
	if (!sentEntities.v.isEmpty()) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_entities;
	}
	const auto replyTo = message.action.replyTo;
	const auto captionText = caption.text;

	if (message.action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
		sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_date;
	} else {
		flags |= MessageFlag::LocalHistoryEntry;
	}

	session->data().registerMessageRandomId(randomId, newId);

	const auto viaBotId = UserId();
	history->addNewLocalMessage(
		newId.msg,
		flags,
		viaBotId,
		replyTo,
		HistoryItem::NewMessageDate(message.action.options.scheduled),
		messageFromId,
		messagePostAuthor,
		media,
		caption,
		HistoryMessageMarkupData());

	auto performRequest = [=](const auto &repeatRequest) -> void {
		auto &histories = history->owner().histories();
		const auto requestType = Data::Histories::RequestType::Send;
		histories.sendRequest(history, requestType, [=](Fn<void()> finish) {
			const auto usedFileReference = media->fileReference();
			history->sendRequestId = api->request(MTPmessages_SendMedia(
				MTP_flags(sendFlags),
				peer->input,
				MTP_int(replyTo),
				inputMedia(),
				MTP_string(captionText),
				MTP_long(randomId),
				MTPReplyMarkup(),
				sentEntities,
				MTP_int(message.action.options.scheduled)
			)).done([=](const MTPUpdates &result) {
				api->applyUpdates(result, randomId);
				finish();
			}).fail([=](const MTP::Error &error) {
				if (error.code() == 400
					&& error.type().startsWith(qstr("FILE_REFERENCE_"))) {
					api->refreshFileReference(origin, [=](const auto &result) {
						if (media->fileReference() != usedFileReference) {
							repeatRequest(repeatRequest);
						} else {
							api->sendMessageFail(error, peer, randomId, newId);
						}
						});
				} else {
					api->sendMessageFail(error, peer, randomId, newId);
				}
				finish();
			}).afterRequest(history->sendRequestId
			).send();
			return history->sendRequestId;
		});
	};
	performRequest(performRequest);

	api->finishForwarding(message.action);
}

} // namespace

void SendExistingDocument(
		Api::MessageToSend &&message,
		not_null<DocumentData*> document) {
	const auto inputMedia = [=] {
		return MTP_inputMediaDocument(
			MTP_flags(0),
			document->mtpInput(),
			MTPint(), // ttl_seconds
			MTPstring()); // query
	};
	SendExistingMedia(
		std::move(message),
		document,
		inputMedia,
		document->stickerOrGifOrigin());

	if (document->sticker()) {
		document->owner().stickers().incrementSticker(document);
	}
}

void SendExistingPhoto(
		Api::MessageToSend &&message,
		not_null<PhotoData*> photo) {
	const auto inputMedia = [=] {
		return MTP_inputMediaPhoto(
			MTP_flags(0),
			photo->mtpInput(),
			MTPint());
	};
	SendExistingMedia(
		std::move(message),
		photo,
		inputMedia,
		Data::FileOrigin());
}

bool SendDice(Api::MessageToSend &message) {
	const auto full = message.textWithTags.text.midRef(0).trimmed();
	auto length = 0;
	if (!Ui::Emoji::Find(full.data(), full.data() + full.size(), &length)
		|| length != full.size()) {
		return false;
	}
	auto &account = message.action.history->session().account();
	auto &config = account.appConfig();
	static const auto hardcoded = std::vector<QString>{
		Stickers::DicePacks::kDiceString,
		Stickers::DicePacks::kDartString,
		Stickers::DicePacks::kSlotString,
		Stickers::DicePacks::kFballString,
		Stickers::DicePacks::kFballString + QChar(0xFE0F),
		Stickers::DicePacks::kBballString,
	};
	const auto list = config.get<std::vector<QString>>(
		"emojies_send_dice",
		hardcoded);
	const auto emoji = full.toString();
	if (!ranges::contains(list, emoji)) {
		return false;
	}
	const auto history = message.action.history;
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto api = &session->api();

	message.textWithTags = TextWithTags();
	message.action.clearDraft = false;
	message.action.generateLocal = true;
	api->sendAction(message.action);

	const auto newId = FullMsgId(
		peerToChannel(peer->id),
		session->data().nextLocalMessageId());
	const auto randomId = base::RandomValue<uint64>();

	auto &histories = history->owner().histories();
	auto flags = NewMessageFlags(peer);
	auto sendFlags = MTPmessages_SendMedia::Flags(0);
	if (message.action.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
		sendFlags |= MTPmessages_SendMedia::Flag::f_reply_to_msg_id;
	}
	const auto replyHeader = NewMessageReplyHeader(message.action);
	const auto anonymousPost = peer->amAnonymous();
	const auto silentPost = ShouldSendSilent(peer, message.action.options);
	InnerFillMessagePostFlags(message.action.options, peer, flags);
	if (silentPost) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_silent;
	}
	auto messageFromId = anonymousPost ? 0 : session->userPeerId();
	auto messagePostAuthor = peer->isBroadcast() ? session->user()->name : QString();
	const auto replyTo = message.action.replyTo;

	if (message.action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
		sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_date;
	} else {
		flags |= MessageFlag::LocalHistoryEntry;
	}

	session->data().registerMessageRandomId(randomId, newId);

	const auto viaBotId = UserId();
	history->addNewLocalMessage(
		newId.msg,
		flags,
		viaBotId,
		message.action.replyTo,
		HistoryItem::NewMessageDate(message.action.options.scheduled),
		messageFromId,
		messagePostAuthor,
		TextWithEntities(),
		MTP_messageMediaDice(MTP_int(0), MTP_string(emoji)),
		HistoryMessageMarkupData());

	const auto requestType = Data::Histories::RequestType::Send;
	histories.sendRequest(history, requestType, [=](Fn<void()> finish) {
		history->sendRequestId = api->request(MTPmessages_SendMedia(
			MTP_flags(sendFlags),
			peer->input,
			MTP_int(replyTo),
			MTP_inputMediaDice(MTP_string(emoji)),
			MTP_string(),
			MTP_long(randomId),
			MTPReplyMarkup(),
			MTP_vector<MTPMessageEntity>(),
			MTP_int(message.action.options.scheduled)
		)).done([=](const MTPUpdates &result) {
			api->applyUpdates(result, randomId);
			finish();
		}).fail([=](const MTP::Error &error) {
			api->sendMessageFail(error, peer, randomId, newId);
			finish();
		}).afterRequest(history->sendRequestId
		).send();
		return history->sendRequestId;
	});
	api->finishForwarding(message.action);
	return true;
}

void FillMessagePostFlags(
		const Api::SendAction &action,
		not_null<PeerData*> peer,
		MessageFlags &flags) {
	InnerFillMessagePostFlags(action.options, peer, flags);
}

void SendConfirmedFile(
		not_null<Main::Session*> session,
		const std::shared_ptr<FileLoadResult> &file) {
	const auto isEditing = (file->type != SendMediaType::Audio)
		&& (file->to.replaceMediaOf != 0);
	const auto channelId = peerToChannel(file->to.peer);

	const auto newId = FullMsgId(
		channelId,
		isEditing
			? file->to.replaceMediaOf
			: session->data().nextLocalMessageId());
	auto groupId = file->album ? file->album->groupId : uint64(0);
	if (file->album) {
		const auto proj = [](const SendingAlbum::Item &item) {
			return item.taskId;
		};
		const auto it = ranges::find(file->album->items, file->taskId, proj);
		Assert(it != file->album->items.end());

		it->msgId = newId;
	}
	session->uploader().upload(newId, file);

	const auto itemToEdit = isEditing
		? session->data().message(newId)
		: nullptr;

	const auto history = session->data().history(file->to.peer);
	const auto peer = history->peer;

	auto action = Api::SendAction(history);
	action.options = file->to.options;
	action.clearDraft = false;
	action.replyTo = file->to.replyTo;
	action.generateLocal = true;
	session->api().sendAction(action);

	auto caption = TextWithEntities{
		file->caption.text,
		TextUtilities::ConvertTextTagsToEntities(file->caption.tags)
	};
	const auto prepareFlags = Ui::ItemTextOptions(
		history,
		session->user()).flags;
	TextUtilities::PrepareForSending(caption, prepareFlags);
	TextUtilities::Trim(caption);
	auto localEntities = Api::EntitiesToMTP(session, caption.entities);

	if (itemToEdit) {
		if (const auto id = itemToEdit->groupId()) {
			groupId = id.value;
		}
	}

	auto flags = isEditing ? MessageFlags() : NewMessageFlags(peer);
	if (file->to.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
	}
	const auto replyHeader = NewMessageReplyHeader(action);
	const auto anonymousPost = peer->amAnonymous();
	const auto silentPost = ShouldSendSilent(peer, file->to.options);
	FillMessagePostFlags(action, peer, flags);
	if (silentPost) {
		flags |= MessageFlag::Silent;
	}
	if (file->to.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;

		// Scheduled messages have no the 'edited' badge.
		flags |= MessageFlag::HideEdited;
	} else {
		flags |= MessageFlag::LocalHistoryEntry;
	}
	if (file->type == SendMediaType::Audio) {
		if (!peer->isChannel() || peer->isMegagroup()) {
			flags |= MessageFlag::MediaIsUnread;
		}
	}

	const auto messageFromId = anonymousPost ? 0 : session->userPeerId();
	const auto messagePostAuthor = peer->isBroadcast()
		? session->user()->name
		: QString();

	const auto media = [&] {
		if (file->type == SendMediaType::Photo) {
			return MTP_messageMediaPhoto(
				MTP_flags(MTPDmessageMediaPhoto::Flag::f_photo),
				file->photo,
				MTPint());
		} else if (file->type == SendMediaType::File) {
			return MTP_messageMediaDocument(
				MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
				file->document,
				MTPint());
		} else if (file->type == SendMediaType::Audio) {
			return MTP_messageMediaDocument(
				MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
				file->document,
				MTPint());
		} else {
			Unexpected("Type in sendFilesConfirmed.");
		}
	}();

	if (itemToEdit) {
		itemToEdit->savePreviousMedia();
		itemToEdit->applyEdition(MTP_message(
			MTP_flags(MTPDmessage::Flag::f_media
				| ((flags & MessageFlag::HideEdited)
					? MTPDmessage::Flag::f_edit_hide
					: MTPDmessage::Flag())
				| (localEntities.v.isEmpty()
					? MTPDmessage::Flag()
					: MTPDmessage::Flag::f_entities)),
			MTP_int(newId.msg),
			peerToMTP(messageFromId),
			peerToMTP(file->to.peer),
			MTPMessageFwdHeader(),
			MTPlong(), // via_bot_id
			replyHeader,
			MTP_int(HistoryItem::NewMessageDate(file->to.options.scheduled)),
			MTP_string(caption.text),
			media,
			MTPReplyMarkup(),
			localEntities,
			MTPint(), // views
			MTPint(), // forwards
			MTPMessageReplies(),
			MTPint(), // edit_date
			MTP_string(messagePostAuthor),
			MTP_long(groupId),
			//MTPMessageReactions(),
			MTPVector<MTPRestrictionReason>(),
			MTPint()).c_message());
	} else {
		const auto viaBotId = UserId();
		history->addNewLocalMessage(
			newId.msg,
			flags,
			viaBotId,
			file->to.replyTo,
			HistoryItem::NewMessageDate(file->to.options.scheduled),
			messageFromId,
			messagePostAuthor,
			caption,
			media,
			HistoryMessageMarkupData(),
			groupId);
	}

	if (isEditing) {
		return;
	}

	session->data().sendHistoryChangeNotifications();
	if (!itemToEdit) {
		session->changes().historyUpdated(
			history,
			(action.options.scheduled
				? Data::HistoryUpdate::Flag::ScheduledSent
				: Data::HistoryUpdate::Flag::MessageSent));
	}
}

} // namespace Api
