/* SPDX-License-Identifier: MIT
 * S9 G3D Power Lab: pure eligibility policy, independently testable.
 * Decision is repeated after acquiring mali_job_lock AND checking the
 * worker queue. Frame age is a rendering activity proxy: screen state
 * is not directly provided by this driver.
 */
#ifndef S9_G3D_LAB_POLICY_H
#define S9_G3D_LAB_POLICY_H
#include <stdint.h>
enum s9_g3d_lab_reason {
 S9_G3D_LAB_ALLOW = 0,
 S9_G3D_LAB_DISABLED,
 S9_G3D_LAB_NOT_READY,
 S9_G3D_LAB_QUEUED,
 S9_G3D_LAB_JOB_RECENT,
 S9_G3D_LAB_FRAME_RECENT,
 S9_G3D_LAB_FAULT,
 S9_G3D_LAB_VOLTAGE,
};
static inline enum s9_g3d_lab_reason
s9_g3d_lab_eligible(unsigned mode, unsigned enabled, unsigned ready,
                    unsigned fault, unsigned voltage_mv, unsigned queued,
                    uint64_t job_age_ms, uint64_t frame_age_ms,
                    unsigned grace_ms)
{
 if (mode > 3 || (mode == 0 && !enabled))
  return S9_G3D_LAB_DISABLED;
 if (!ready)
  return S9_G3D_LAB_NOT_READY;
 if (fault)
  return S9_G3D_LAB_FAULT;
 if (voltage_mv != 950)
  return S9_G3D_LAB_VOLTAGE;
 if (queued)
  return S9_G3D_LAB_QUEUED;
 if (job_age_ms < grace_ms)
  return S9_G3D_LAB_JOB_RECENT;
 if (frame_age_ms < grace_ms)
  return S9_G3D_LAB_FRAME_RECENT;
 return S9_G3D_LAB_ALLOW;
}
/* Conservative explicit voltage steps for the first S9 experiment.
 * No lower voltage or APM-table-driven transition is authorized here. */
static inline int s9_g3d_lab_mv_allowed(unsigned mv) {
 return mv == 950 || mv == 925 || mv == 900;
}
/* Round requested millivolts UP to PMIC 6.25-mV steps; upper bound 950mV. */
static inline unsigned s9_g3d_lab_vsel(unsigned mv) {
 return mv <= 300 ? 0u : (mv - 300u) * 4u / 25u +
     (((mv - 300u) * 4u) % 25u != 0u);
}
static inline unsigned s9_g3d_lab_mv_from_vsel(unsigned sel) {
 return 300u + sel * 25u / 4u;
}
#endif
