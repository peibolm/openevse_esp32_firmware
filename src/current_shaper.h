#ifndef _OPENEVSE_CUR_SHAPER_H
#define _OPENEVSE_CUR_SHAPER_H

// Time between loop polls
#ifndef EVSE_SHAPER_LOOP_TIME
#define EVSE_SHAPER_LOOP_TIME 2000
#endif

#ifndef EVSE_SHAPER_HYSTERESIS
#define EVSE_SHAPER_HYSTERESIS 0.5 // A
#endif

// How close the EV's measured current has to be to our last commanded current
// before we trust it has finished ramping.
#ifndef EVSE_SHAPER_SETTLE_TOLERANCE
#define EVSE_SHAPER_SETTLE_TOLERANCE 1.0 // A
#endif

// Tau for smoothing the safety/pause decision only (independent of
// current_shaper_smoothing_time, which governs the separate paused-state
// resume smoothing below). Kept at InputFilter's enforced minimum (10s) to
// stay as responsive as possible while still rejecting multi-second
// transient spikes (e.g. an EV's precharge/self-test pulse at charge start).
#ifndef EVSE_SHAPER_SAFETY_FILTER_TAU
#define EVSE_SHAPER_SAFETY_FILTER_TAU 10 // sec
#endif

// Safety valve: if the EV never quite reaches the commanded current (e.g. its
// own onboard limit), stop waiting for it after this long so headroom doesn't
// go permanently unused.
#ifndef EVSE_SHAPER_SETTLE_TIMEOUT
#define EVSE_SHAPER_SETTLE_TIMEOUT 30 // sec
#endif

#include "emonesp.h"
#include <MicroTasks.h>
#include "evse_man.h"
#include "app_config.h"
#include "http_update.h"
#include "input.h"
#include "event.h"
#include "divert.h"
#include "input_filter.h"

class CurrentShaperTask: public MicroTasks::Task
{
  private:
    EvseManager *_evse;
    bool         _enabled;
    bool         _changed;
    int          _max_pwr;   // total current available from the grid
    int          _live_pwr;  // current available to EVSE
    double       _smoothed_live_pwr; // filtered live power for getting out of pause only
    uint8_t      _chg_cur;   // calculated charge current to claim
    double       _max_cur;   // shaper calculated max current
    uint32_t     _timer;
    uint32_t     _pause_timer;
    bool         _updated;
    // Set after every claimed change, cleared on the next live-power reading.
    // Blocks normal (non-pause) adjustments from acting on a house-power
    // figure that predates the EV's response to our last change.
    bool         _awaiting_fresh_reading;
    // Last max current we actually claimed, and when. Increases (only) wait
    // for evse.getAmps() to catch up to this before claiming a further
    // increase - decreases and pausing always act immediately. See loop().
    double       _last_claimed_cur;
    uint32_t     _last_change_time;
    InputFilter  _inputFilter;
    // Short-tau smoothed live power, used only for the pause/over-budget
    // entry decision in loop() - independent of _smoothed_live_pwr above,
    // which only tracks once already paused and is biased for that purpose.
    double       _safety_live_pwr;
    bool         _safety_filter_seeded;
    double       _safety_max_cur;   // max current computed from _safety_live_pwr
    InputFilter  _safetyFilter;     // separate instance: InputFilter tracks _last_data_time per-instance

  protected:
    void setup();
    unsigned long loop(MicroTasks::WakeReason reason);
    double calcMaxCur(double livepwr);

  public:
    CurrentShaperTask();
    ~CurrentShaperTask();
    void begin(EvseManager &evse);
    void shapeCurrent();
    void setMaxPwr(int max_pwr);
    void setLivePwr(int live_pwr);
    void setState(bool state);
    bool getState();
    int getMaxPwr();
    int getLivePwr();
    int getSmoothedLivePwr();
    double getMaxCur();
    bool isActive();
    bool isUpdated();

    void notifyConfigChanged(bool enabled, uint32_t max_pwr);
};

extern CurrentShaperTask shaper;

#endif // CURRENT_SHAPER
