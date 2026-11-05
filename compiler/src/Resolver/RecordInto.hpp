#pragma once
// Part of the resolver: GlobalScopeImpl.hpp includes it where it was.

// Records what the enclosed work reads into target, or nothing (null): the reads of a cached
// result's computation belong to that result, not to whoever happened to compute it first.
struct RecordInto
{
  explicit RecordInto(Dependencies* into) : target(into), saved(recordingInto)
  {
    if (!recordingDependencies)
    {
      target = nullptr; // nothing is recorded, and recordingInto stays null
      return;
    }
    recordingInto = into ? &part : nullptr;
    lastName = UINT32_MAX;
    lastMember = 0;
    lastUse = nullptr;
    if (into && !recentUses)
      recentUses = std::make_unique<RecentUses>();
    savedUnit = recordingUnit;
    recordingUnit = ++unitCounter;
    savedReporting = reportingUnit;
    reportingUnit = into;
  }
  ~RecordInto()
  {
    if (!recordingDependencies)
      return;
    recordingInto = saved;
    reportingUnit = savedReporting;
    lastName = UINT32_MAX;
    lastMember = 0;
    lastUse = nullptr;
    recordingUnit = ++unitCounter; // the outer unit's remembered uses may have been overwritten
    (void)savedUnit;
    if (target)
      target->add(part);
  }
  RecordInto(const RecordInto&) = delete;

  Dependencies* target;
  DependencyList* saved;
  DependencyList part;
  uint32_t savedUnit = 0;
  const void* savedReporting = nullptr;
  RecordInto& operator=(const RecordInto&) = delete;
};
