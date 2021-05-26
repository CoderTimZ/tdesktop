/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {

class AdaptiveModern {
public:
	enum class WindowLayout {
		OneColumn,
		Normal,
		ThreeColumn,
	};

	enum class ChatLayout {
		Normal,
		Wide,
	};

	AdaptiveModern();

	void setWindowLayout(WindowLayout value);
	void setChatLayout(ChatLayout value);

	[[nodiscard]] rpl::producer<> changed() const;
	[[nodiscard]] rpl::producer<bool> oneColumnValue() const;
	[[nodiscard]] rpl::producer<ChatLayout> chatLayoutValue() const;

	[[nodiscard]] bool isOneColumn() const;
	[[nodiscard]] bool isNormal() const;
	[[nodiscard]] bool isThreeColumn() const;

private:
	rpl::variable<ChatLayout> _chatLayout;
	rpl::variable<WindowLayout> _layout;

};

} // namespace Window