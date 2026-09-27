// 飞行道具追踪：直接读游戏里飞行道具（SBProjectile 对象池）的实时位置，
// 用最近一小段的位移算方向和速度（速度大小夹到道具表的初速~最高速），解出什么时候飞到伊芙身上。不依赖出发点，追踪弹也适用
#pragma once
#include "common.h"

struct ProjHit {
    uint64_t addr; // 道具实例
    double t;      // 距现在多少秒碰到伊芙（已经碰到为 0）
    bool jp, ja;   // 可完美弹反 / 可完美闪避（道具表）
    float speed;   // 当前速度（米/秒）
    float dmin;    // 预计最近距离（米）
    float dist;    // 当前距离（米）
};

void ResetProjectiles();                     // 游戏断开时清空
// 每帧：正在飞、会打到伊芙的道具放进 out；返回当前有没有道具在飞（打不到伊芙的也算）
bool ProjectilesTick(std::vector<ProjHit>& out);
