/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <core/OverlayRules.h>
#include <gtest/gtest.h>

using namespace ts4_mod::overlay;

TEST(OverlayVisibility, ShowsWhileTheMenuOfTheGameIsOpen)
{
	OverlayVisibility visibility;
	EXPECT_EQ(visibility.Update(false, false), (VisibilityChange {}));
	EXPECT_EQ(visibility.Update(true, false), (VisibilityChange {.visible = true, .shown = true}));
	EXPECT_EQ(visibility.Update(true, false), (VisibilityChange {.visible = true}));
	EXPECT_EQ(visibility.Update(false, false), (VisibilityChange {.hidden = true}));
	EXPECT_EQ(visibility.Update(false, false), (VisibilityChange {}));
}

TEST(OverlayVisibility, ThePinKeepsTheBarOpenUntilTheNextPin)
{
	OverlayVisibility visibility;
	EXPECT_EQ(visibility.Update(false, true), (VisibilityChange {.visible = true, .shown = true}));
	EXPECT_TRUE(visibility.Pinned());
	// The menu of the game opens and closes; the pinned bar stays.
	EXPECT_EQ(visibility.Update(true, false), (VisibilityChange {.visible = true}));
	EXPECT_EQ(visibility.Update(false, false), (VisibilityChange {.visible = true}));
	EXPECT_EQ(visibility.Update(false, true), (VisibilityChange {.hidden = true}));
	EXPECT_FALSE(visibility.Pinned());
	// A pin while the menu is open changes nothing that shows, but the bar stays after the menu closes.
	(void)visibility.Update(true, false);
	EXPECT_EQ(visibility.Update(true, true), (VisibilityChange {.visible = true}));
	EXPECT_EQ(visibility.Update(false, false), (VisibilityChange {.visible = true}));
}

TEST(OverlayVisibility, HidesInTheFrameEachTimeTheMenuCloses)
{
	OverlayVisibility visibility;
	for (int cycle = 0; cycle < 3; ++cycle)
	{
		EXPECT_EQ(visibility.Update(true, false), (VisibilityChange {.visible = true, .shown = true})) << cycle;
		EXPECT_EQ(visibility.Update(true, false), (VisibilityChange {.visible = true})) << cycle;
		EXPECT_EQ(visibility.Update(false, false), (VisibilityChange {.hidden = true})) << cycle;
		EXPECT_FALSE(visibility.Visible()) << cycle;
	}
	// The pin, then the menu: the bar hides only at the second pin.
	EXPECT_EQ(visibility.Update(false, true), (VisibilityChange {.visible = true, .shown = true}));
	EXPECT_EQ(visibility.Update(true, false), (VisibilityChange {.visible = true}));
	EXPECT_EQ(visibility.Update(true, true), (VisibilityChange {.visible = true}));
	EXPECT_EQ(visibility.Update(false, false), (VisibilityChange {.hidden = true}));
}

TEST(OverlayVisibility, ADisabledBarNeverShows)
{
	OverlayVisibility visibility;
	(void)visibility.Update(true, false);
	visibility.SetEnabled(false);
	EXPECT_EQ(visibility.Update(true, false), (VisibilityChange {.hidden = true}));
	EXPECT_EQ(visibility.Update(true, true), (VisibilityChange {}));
	visibility.SetEnabled(true);
	EXPECT_EQ(visibility.Update(true, false), (VisibilityChange {.visible = true, .shown = true}));
}

TEST(OverlayInput, ClassifiesTheWindowMessages)
{
	EXPECT_EQ(ClassifyMessage(wm::k_MouseMove), InputKind::MouseMove);
	EXPECT_EQ(ClassifyMessage(wm::k_LeftButtonDown), InputKind::MouseButtonDown);
	EXPECT_EQ(ClassifyMessage(wm::k_RightButtonDoubleClick), InputKind::MouseButtonDown);
	EXPECT_EQ(ClassifyMessage(wm::k_MiddleButtonUp), InputKind::MouseButtonUp);
	EXPECT_EQ(ClassifyMessage(wm::k_MouseWheel), InputKind::MouseWheel);
	EXPECT_EQ(ClassifyMessage(wm::k_SysKeyDown), InputKind::KeyDown);
	EXPECT_EQ(ClassifyMessage(wm::k_KeyUp), InputKind::KeyUp);
	EXPECT_EQ(ClassifyMessage(wm::k_Char), InputKind::Character);
	EXPECT_EQ(ClassifyMessage(wm::k_Input), InputKind::RawInput);
	EXPECT_EQ(ClassifyMessage(wm::k_KillFocus), InputKind::Focus);
	EXPECT_EQ(ClassifyMessage(0x0005), InputKind::Other);
}

TEST(OverlayInput, TheGameGetsAllWhileTheBarDoesNotShow)
{
	for (const auto kind : {InputKind::MouseMove, InputKind::MouseButtonDown, InputKind::KeyDown, InputKind::RawInput})
	{
		EXPECT_EQ(DecideInput(kind, 0, false, true, true), (InputDecision {}));
	}
}

TEST(OverlayInput, TheBarTakesTheMouseOnlyWhileTheMouseIsOnIt)
{
	for (const auto kind : {InputKind::MouseMove, InputKind::MouseButtonDown, InputKind::MouseWheel, InputKind::RawInput})
	{
		EXPECT_EQ(DecideInput(kind, 0, true, true, false), (InputDecision {.feedOverlay = true, .swallow = true}));
		EXPECT_EQ(DecideInput(kind, 0, true, false, false), (InputDecision {.feedOverlay = true}));
	}
	// The release of a button always goes to the game, so that no button stays down.
	EXPECT_EQ(DecideInput(InputKind::MouseButtonUp, 0, true, true, true), (InputDecision {.feedOverlay = true}));
}

TEST(OverlayInput, TheBarTakesTheKeysOnlyWhileAControlHasTheKeyboard)
{
	EXPECT_EQ(DecideInput(InputKind::KeyDown, 'A', true, false, true), (InputDecision {.feedOverlay = true, .swallow = true}));
	EXPECT_EQ(DecideInput(InputKind::Character, 'a', true, false, true),
	          (InputDecision {.feedOverlay = true, .swallow = true}));
	EXPECT_EQ(DecideInput(InputKind::KeyDown, 'A', true, true, false), (InputDecision {.feedOverlay = true}));
	// Escape always goes to the game, which closes its menu.
	EXPECT_EQ(DecideInput(InputKind::KeyDown, wm::k_EscapeKey, true, true, true), (InputDecision {.feedOverlay = true}));
	EXPECT_EQ(DecideInput(InputKind::KeyUp, 'A', true, true, true), (InputDecision {.feedOverlay = true}));
	EXPECT_EQ(DecideInput(InputKind::Focus, 0, true, true, true), (InputDecision {.feedOverlay = true}));
}

TEST(OverlayInput, OnlyTheGameSetsTheCursor)
{
	EXPECT_EQ(ClassifyMessage(wm::k_SetCursor), InputKind::Cursor);
	// ImGui does not get the message, and the game gets it, also while the mouse is on the bar.
	EXPECT_EQ(DecideInput(InputKind::Cursor, 0, true, true, true), (InputDecision {}));
	EXPECT_EQ(DecideInput(InputKind::Cursor, 0, false, false, false), (InputDecision {}));
}
