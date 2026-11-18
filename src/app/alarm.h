// alarm.h -- 闹钟规则（对应 Go alarm/alarm.go + conf/conf.go 的 WorkDayApi + timeJob）
//
// 工作日判定顺序严格照 Go：
//   1. https://timor.tech/api/holiday/info/YYYY-MM-DD
//      code==0 时取 type.type：0 普通工作日 / 1 普通周末 / 2 法定节假日 / 3 调休补班
//      IsWorkDay = (type == 0 || type == 3)
//   2. 接口失败：当天有缓存用缓存，没有就按"非周六日"兜底，并每分钟重试
//   3. 大小周：smallWeekDate 是小周日期，过了就 +14 天起按周找下一个非节假日/非补班的
//      普通日子写回配置；今天等于它则强制工作日
#pragma once

#include <Arduino.h>

void alarm_begin();
void alarm_tick();  // 每秒调一次（内部按分钟去重）；wake 命令也调它

// 对齐 Go 的 alarm.Add / alarm.DelTypes；msg 回给界面用（失败也给原因）
bool alarm_add(const String &hhmm, const String &types, String *msg);
bool alarm_del_types(const String &hhmm, const String &types, String *msg);
String alarm_list_json();
