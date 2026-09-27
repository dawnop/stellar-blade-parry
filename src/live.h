// 实时状态：游戏窗口、玩家/敌人/相机（每帧顺着 PlayerController 指针读）
#pragma once
#include "common.h"

struct Live {
    uint64_t pc = 0, pcm = 0, player = 0, enemy = 0; // PlayerController / 相机管理器 / 伊芙 / 锁定或看着的敌人
    uint64_t lastEnemy = 0; // 最近锁定过的敌人（Boss 的爆发招会解除锁定，距离还要接着算）
};
extern Live g_live;
extern HWND g_gameWnd;

void LiveTick();                                  // 每帧调用
bool GameClient(RECT& r);                         // 游戏客户区（屏幕坐标）
bool GameForeground();                            // 游戏窗口在前台
bool ActorLocation(uint64_t actor, float out[3]); // 世界坐标（厘米）
bool CameraPov(float loc[3], float rot[3], float& fov);
bool Project(const float w[3], int& sx, int& sy); // 世界 -> 屏幕
uint64_t TargetEnemy();                           // 锁定的敌人，没有就用最近锁定过的
float TargetDistance();                           // 伊芙到 TargetEnemy 的水平距离（米），未知返回 -1
float EveTimeScale();                             // 伊芙的时间流速 = 世界 TimeDilation × 自身 CustomTimeDilation
int CutsceneQteAction();                          // 过场 QTE 正在等输入时返回所需动作的 FName 序号，否则 -1
float EveHpPercent();                             // 伊芙血条（0~1），未知返回 -1
bool EnemyGroggy();                               // TargetEnemy 处于可惩戒（Groggy）状态
