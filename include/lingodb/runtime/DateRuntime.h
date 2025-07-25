#ifndef LINGODB_RUNTIME_DATERUNTIME_H
#define LINGODB_RUNTIME_DATERUNTIME_H
#include "lingodb/runtime/helpers.h"
namespace lingodb::runtime {
struct DateRuntime {
   static int64_t extractHour(int64_t date);
   static int64_t extractMinute(int64_t date);
   static int64_t extractSecond(int64_t date);
   static int64_t extractDay(int64_t date);
   static int64_t extractMonth(int64_t date);
   static int64_t extractYear(int64_t date);
   static int64_t subtractMonths(int64_t date, int64_t months);
   static int64_t addMonths(int64_t date, int64_t months);
   static int64_t dateDiffSecond(int64_t start, int64_t end);
   static int64_t dateDiffMinute(int64_t start, int64_t end);
   static int64_t dateDiffHour(int64_t start, int64_t end);
   static int64_t dateDiffDay(int64_t start, int64_t end);
   static int64_t dateTrunc(VarLen32 part, int64_t date);
};
} // namespace lingodb::runtime
#endif // LINGODB_RUNTIME_DATERUNTIME_H
