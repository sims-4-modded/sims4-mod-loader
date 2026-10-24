/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <cstdint>

#include <array>
#include <string>

#include <core/HookRules.h>
#include <gtest/gtest.h>
#include <mod_api/Ts4ModApi.h>

using namespace ts4_mod::overlay;
using ts4_mod::loader::CheckSameFunction;
using ts4_mod::loader::GameModuleName;
using ts4_mod::loader::MakeFunctionTarget;

namespace
{
constexpr std::array<uintptr_t, 2> k_OwnHooks {0x1000, 0x2000};
} // namespace

TEST(HookRules, AcceptsAFunctionOfTheVtableModule)
{
	EXPECT_EQ(CheckHookTarget(0x7000, k_OwnHooks, true), HookTargetError::None);
}

TEST(HookRules, RefusesAnEmptySlot)
{
	EXPECT_EQ(CheckHookTarget(0, k_OwnHooks, true), HookTargetError::NoFunction);
}

TEST(HookRules, NeverKeepsAHookOfTheModAsTheOriginal)
{
	// A slot that holds a hook of the extender must never become the original function, also in the module of the vtable.
	EXPECT_EQ(CheckHookTarget(0x1000, k_OwnHooks, true), HookTargetError::OwnHook);
	EXPECT_EQ(CheckHookTarget(0x2000, k_OwnHooks, false), HookTargetError::OwnHook);
}

TEST(HookRules, RefusesAFunctionOutsideTheVtableModule)
{
	EXPECT_EQ(CheckHookTarget(0x7000, k_OwnHooks, false), HookTargetError::OutsideModule);
}

TEST(HookRules, OnlyTheOuterCallIsOuter)
{
	HookDepth depth;
	{
		const HookScope outer(depth);
		EXPECT_TRUE(outer.Outer());
		EXPECT_EQ(depth.Depth(), 1);
		{
			const HookScope inner(depth);
			EXPECT_FALSE(inner.Outer());
			EXPECT_EQ(depth.Depth(), 2);
		}
		EXPECT_EQ(depth.Depth(), 1);
	}
	EXPECT_EQ(depth.Depth(), 0);
	const HookScope next(depth);
	EXPECT_TRUE(next.Outer());
}

TEST(HookRules, FindsALoopOneTime)
{
	HookDepth depth;
	int loops = 0;
	for (int call = 1; call <= k_HookLoopDepth + 4; ++call)
	{
		const auto entry = depth.Enter();
		EXPECT_EQ(entry.outer, call == 1);
		loops += entry.loopFound ? 1 : 0;
		EXPECT_EQ(entry.loopFound, call == k_HookLoopDepth);
	}
	for (int call = 0; call < k_HookLoopDepth + 4; ++call)
	{
		depth.Leave();
	}
	EXPECT_EQ(depth.Depth(), 0);
	EXPECT_EQ(loops, 1);
	// A second loop on the same thread is not told again.
	for (int call = 0; call < k_HookLoopDepth; ++call)
	{
		EXPECT_FALSE(depth.Enter().loopFound);
	}
}

TEST(HookRules, LeaveNeverGoesBelowZero)
{
	HookDepth depth;
	depth.Leave();
	EXPECT_EQ(depth.Depth(), 0);
	EXPECT_TRUE(depth.Enter().outer);
}

TEST(HookRules, ANormalChainHasNoLoop)
{
	// A hook on Present that DXGI calls one more time (for example from another function of the swap chain) is a
	// depth of two. It is not a loop.
	HookDepth depth;
	EXPECT_FALSE(depth.Enter().loopFound);
	EXPECT_FALSE(depth.Enter().loopFound);
	depth.Leave();
	depth.Leave();
	EXPECT_EQ(depth.Depth(), 0);
}

TEST(FunctionRules, AFunctionKeyIsTheModuleAndTheNormalSignature)
{
	const auto target = MakeFunctionTarget("simulation_x64.DLL", "ZoneLoad", "48 89 5c 24 ? 57", 2);
	ASSERT_TRUE(target) << target.GetError().message;
	EXPECT_EQ(target->key.module, TS4_MODULE_SIMULATION);
	EXPECT_EQ(target->key.signature, "48 89 5C 24 ?? 57");
	EXPECT_EQ(target->key.Text(), "Simulation_x64.dll [48 89 5C 24 ?? 57]");
	EXPECT_EQ(target->name, "ZoneLoad");
	EXPECT_EQ(target->parameterCount, 2u);
	// Two texts of the same signature give the same key.
	const auto same = MakeFunctionTarget(TS4_MODULE_SIMULATION, "Other", "48  89 5C 24 ?? 57", 2);
	ASSERT_TRUE(same);
	EXPECT_EQ(same->key, target->key);
	EXPECT_EQ(GameModuleName("ts4_x64.exe"), TS4_MODULE_GAME);
	EXPECT_TRUE(GameModuleName("python37_x64.dll").empty());
}

TEST(FunctionRules, RefusesABadModuleNameSignatureOrParameterCount)
{
	EXPECT_FALSE(MakeFunctionTarget("python37_x64.dll", "Eval", "48 89", 0));
	EXPECT_FALSE(MakeFunctionTarget(TS4_MODULE_GAME, "", "48 89", 0));
	EXPECT_FALSE(MakeFunctionTarget(TS4_MODULE_GAME, std::string(129, 'a'), "48 89", 0));
	EXPECT_FALSE(MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "", 0));
	EXPECT_FALSE(MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "?? ??", 0));
	EXPECT_FALSE(MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "48 8", 0));
	const auto many = MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "48 89", TS4_CALL_MAX_ARGUMENTS + 1);
	ASSERT_FALSE(many);
	EXPECT_NE(many.GetError().message.find("at most 4"), std::string::npos);
	EXPECT_TRUE(MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "48 89", TS4_CALL_MAX_ARGUMENTS));
}

TEST(FunctionRules, AllCallbacksOnOneFunctionAgreeOnTheParameterCount)
{
	const auto first = *MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "48 89 ??", 1);
	EXPECT_TRUE(CheckSameFunction(first, *MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "48 89 ??", 1), "Needs"));
	const auto other = CheckSameFunction(first, *MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "48 89 ??", 2), "Needs");
	ASSERT_FALSE(other);
	EXPECT_NE(other.GetError().message.find("Needs"), std::string::npos);
	// A different function can have a different count.
	EXPECT_TRUE(CheckSameFunction(first, *MakeFunctionTarget(TS4_MODULE_SIMULATION, "Tick", "48 89 ??", 2), "Needs"));
	EXPECT_TRUE(CheckSameFunction(first, *MakeFunctionTarget(TS4_MODULE_GAME, "Tick", "48 8B ??", 2), "Needs"));
}
