/*
	Adapted from the ScriptHookRDR2 SDK's NativeTrainer sample (Alexander
	Blade, http://dev-c.com). Same two changes CollectorOffline made, both
	forced by building against a newer C++ standard than the 2019 sample
	targeted -- the F10-vs-F5 toggle logic itself lives in scriptmenu.h's
	MenuInput::MenuSwitchPressed(), not here:
	  - DrawText renamed DrawTextAt: windows.h #defines DrawText to
	    DrawTextA (Win32's own function) when built with the MultiByte
	    character set, which silently swallowed this function's own
	    definition/calls into calling the wrong (Win32) function entirely.
	  - const_cast at the one call site that hands a literal to a stock
	    native declared char* (not const char*) -- invoke<> doesn't care
	    about constness, it's a pass-by-value template, so this is safe.
*/

#include "scriptmenu.h"

void DrawTextAt(float x, float y, const char *str)
{
	UI::DRAW_TEXT(GAMEPLAY::CREATE_STRING(10, const_cast<char*>("LITERAL_STRING"), const_cast<char*>(str)), x, y);
}

void DrawRect(float lineLeft, float lineTop, float lineWidth, float lineHeight, int r, int g, int b, int a)
{
	GRAPHICS::DRAW_RECT((lineLeft + (lineWidth * 0.5f)), (lineTop + (lineHeight * 0.5f)), lineWidth, lineHeight, r, g, b, a, 0, 0);
}

void MenuItemBase::WaitAndDraw(int ms)
{
	DWORD time = GetTickCount() + ms;
	bool waited = false;
	while (GetTickCount() < time || !waited)
	{
		WAIT(0);
		waited = true;
		if (auto menu = GetMenu())
			menu->OnDraw();
	}
}

void MenuItemBase::SetStatusText(string text, int ms)
{
	MenuController *controller;
	if (m_menu && (controller = m_menu->GetController()))
		controller->SetStatusText(text, ms);
}

void MenuItemBase::OnDraw(float lineTop, float lineLeft, bool active)
{
	// text
	ColorRgba color = active ? m_colorTextActive : m_colorText;
	UI::SET_TEXT_SCALE(0.0, m_lineHeight * 8.0f);
	UI::SET_TEXT_COLOR_RGBA(color.r, color.g, color.b, color.a);
	UI::SET_TEXT_CENTRE(0);
	UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
	DrawTextAt(lineLeft + m_textLeft, lineTop + m_lineHeight / 4.5f, GetCaption().c_str());
	// rect
	color = active ? m_colorRectActive : m_colorRect;
	DrawRect(lineLeft, lineTop, m_lineWidth, m_lineHeight, color.r, color.g, color.b, color.a);
}

void MenuItemSwitchable::OnDraw(float lineTop, float lineLeft, bool active)
{
	MenuItemDefault::OnDraw(lineTop, lineLeft, active);
	float lineWidth = GetLineWidth();
	float lineHeight = GetLineHeight();
	ColorRgba color = active ? GetColorTextActive() : GetColorText();
	UI::SET_TEXT_SCALE(0.0, lineHeight * 8.0f);
	UI::SET_TEXT_COLOR_RGBA(color.r, color.g, color.b, static_cast<int>(color.a / 1.1f));
	UI::SET_TEXT_CENTRE(0);
	UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
	DrawTextAt(lineLeft + lineWidth - lineWidth / 6.35f, lineTop + lineHeight / 4.8f, GetState() ? "[Y]" : "[N]");
}

void MenuItemMenu::OnDraw(float lineTop, float lineLeft, bool active)
{
	MenuItemDefault::OnDraw(lineTop, lineLeft, active);
	float lineWidth = GetLineWidth();
	float lineHeight = GetLineHeight();
	ColorRgba color = active ? GetColorTextActive() : GetColorText();
	UI::SET_TEXT_SCALE(0.0, lineHeight * 8.0f);
	UI::SET_TEXT_COLOR_RGBA(color.r, color.g, color.b, color.a / 2);
	UI::SET_TEXT_CENTRE(0);
	UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
	DrawTextAt(lineLeft + lineWidth - lineWidth / 8, lineTop + lineHeight / 3.5f, "*");
}

void MenuItemMenu::OnSelect()
{
	if (auto parentMenu = GetMenu())
		if (auto controller = parentMenu->GetController())
			controller->PushMenu(m_menu);
}

void MenuBase::OnDraw()
{
	float lineTop = MenuBase_menuTop;
	float lineLeft = MenuBase_menuLeft;
	if (m_itemTitle->GetClass() == eMenuItemClass::ListTitle)
		reinterpret_cast<MenuItemListTitle *>(m_itemTitle)->
			SetCurrentItemInfo(GetActiveItemIndex() + 1, static_cast<int>(m_items.size()));
	m_itemTitle->OnDraw(lineTop, lineLeft, false);
	lineTop += m_itemTitle->GetLineHeight();
	for (int i = 0; i < MenuBase_linesPerScreen; i++)
	{
		int itemIndex = m_activeScreenIndex * MenuBase_linesPerScreen + i;
		if (itemIndex == m_items.size())
			break;
		MenuItemBase *item = m_items[itemIndex];
		item->OnDraw(lineTop, lineLeft, m_activeLineIndex == i);
		lineTop += item->GetLineHeight() - item->GetLineHeight() * MenuBase_lineOverlap;
	}
}

int MenuBase::OnInput()
{
	const int itemCount = static_cast<int>(m_items.size());
	const int itemsLeft = itemCount % MenuBase_linesPerScreen;
	const int screenCount = itemCount / MenuBase_linesPerScreen + (itemsLeft ? 1 : 0);
	const int lineCountLastScreen = itemsLeft ? itemsLeft : MenuBase_linesPerScreen;

	auto buttons = MenuInput::GetButtonState();

	int waitTime = 0;

	if (buttons.a || buttons.b || buttons.up || buttons.down)
	{
		MenuInput::MenuInputBeep();
		waitTime = buttons.b ? 200 : 150;
	}

	if (buttons.a)
	{
		int activeItemIndex = GetActiveItemIndex();
		m_items[activeItemIndex]->OnSelect();
	} else
	if (buttons.b)
	{
		if (auto controller = GetController())
			controller->PopMenu();
	} else
	if (buttons.up)
	{
		if (m_activeLineIndex-- == 0)
		{
			if (m_activeScreenIndex == 0)
			{
				m_activeScreenIndex = screenCount - 1;
				m_activeLineIndex = lineCountLastScreen - 1;
			} else
			{
				m_activeScreenIndex--;
				m_activeLineIndex = MenuBase_linesPerScreen - 1;
			}
		}
	} else
	if (buttons.down)
	{
		m_activeLineIndex++;
		if (m_activeLineIndex == ((m_activeScreenIndex == (screenCount - 1)) ? lineCountLastScreen : MenuBase_linesPerScreen))
		{
			if (m_activeScreenIndex == screenCount - 1)
				m_activeScreenIndex = 0;
			else
				m_activeScreenIndex++;
			m_activeLineIndex = 0;
		}
	}

	return waitTime;
}

void MenuController::DrawStatusText()
{
	if (GetTickCount() < m_statusTextMaxTicks)
	{
		UI::SET_TEXT_SCALE(0.55, 0.55);
		UI::SET_TEXT_COLOR_RGBA(255, 255, 255, 255);
		UI::SET_TEXT_CENTRE(1);
		UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
		DrawTextAt(0.5, 0.5, m_statusText.c_str());
	}
}
