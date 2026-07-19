#include "current_shaper.h"
#include "input_filter.h"

//global instance
CurrentShaperTask shaper;

CurrentShaperTask::CurrentShaperTask() : MicroTasks::Task() {
	_changed = false;
	_enabled = false;
	_max_pwr = 0;
	_live_pwr = 0;
	_smoothed_live_pwr = 0;
	_chg_cur = 0;
	_max_cur = 0;
	_pause_timer = 0;
	_timer = 0;
	_updated = false;
	_awaiting_fresh_reading = false;
	_last_claimed_cur = 0;
	_last_change_time = 0;
	_safety_live_pwr = 0;
	_safety_filter_seeded = false;
	_safety_max_cur = 0;
}

CurrentShaperTask::~CurrentShaperTask() {
	// should be useless but just in case
	evse.release(EvseClient_OpenEVSE_Shaper);
}

void CurrentShaperTask::setup() {

}

unsigned long CurrentShaperTask::loop(MicroTasks::WakeReason reason) {

	if (_enabled) {
			EvseProperties props;
			if (_changed) {
				props.setMaxCurrent(floor(_max_cur));
				bool pausing = _safety_max_cur < evse.getMinCurrent();
				if (pausing) {
					// pause temporary, not enough amps available
					props.setState(EvseState::Disabled);
					if (!_pause_timer)
					{
						_pause_timer = millis();
					}

				}
				else if (millis() - _pause_timer >= current_shaper_min_pause_time * 1000 && (_max_cur - evse.getMinCurrent() >= EVSE_SHAPER_HYSTERESIS))
				{
					_pause_timer = 0;
					props.setState(EvseState::None);
				}
				_timer = millis();

				bool decreasing = !pausing && (floor(_max_cur) < _last_claimed_cur);
				// Only a decrease that's needed *right now* to get back under
				// budget is safety-relevant. A decrease while still under budget
				// is just the same still-ramping EV inflating the reading - not
				// an emergency, so it gets the same gate as an increase below.
				// pausing/over_budget are both based on the short-tau-smoothed
				// _safety_live_pwr/_safety_max_cur, not the raw reading, so a
				// multi-second transient spike (e.g. an EV's precharge/self-test
				// pulse at charge start) can't trigger them on a single sample.
				bool over_budget = _safety_live_pwr >= _max_pwr;
				bool settled = fabs(evse.getAmps() - _last_claimed_cur) <= EVSE_SHAPER_SETTLE_TOLERANCE
				            || millis() - _last_change_time > EVSE_SHAPER_SETTLE_TIMEOUT * 1000;

				// Pausing, and a decrease while genuinely over budget, are
				// safety-relevant and always act immediately. Everything else
				// (an increase, or a precautionary decrease while still under
				// budget) waits for both a live-power reading that arrived after
				// our last change AND the EV's measured current to have caught up
				// to it, so we never react to a still-ramping/stale picture.
				if (pausing || (decreasing && over_budget) || (!_awaiting_fresh_reading && settled))
				{
					_changed = false;
					// claim only if we have change
					if (evse.getState() != props.getState() || evse.getChargeCurrent() != props.getChargeCurrent())
					{
						evse.claim(EvseClient_OpenEVSE_Shaper, EvseManager_Priority_Safety, props);
						_awaiting_fresh_reading = true;
						_last_claimed_cur = floor(_max_cur);
						_last_change_time = millis();
						StaticJsonDocument<192> event;
						event["shaper"] = 1;
						event["shaper_live_pwr"] = _live_pwr;
						event["shaper_smoothed_live_pwr"] = _smoothed_live_pwr;
						event["shaper_safety_live_pwr"] = _safety_live_pwr;
						event["shaper_max_pwr"] = _max_pwr;
						event["shaper_cur"] = _max_cur;
						event["shaper_updated"] = _updated;
						event_send(event);
					}
				}
			}
			else if ( !_updated || millis() - _timer > current_shaper_data_maxinterval * 1000 )
			{
				//available power has not been updated since EVSE_SHAPER_FAILSAFE_TIME, pause charge
				DBUGF("shaper_live_pwr has not been updated in time, pausing charge");

				if (_updated)
				{
					_pause_timer = millis();
					_updated = false;
					_smoothed_live_pwr = _live_pwr;
				}

				if (evse.getState(EvseClient_OpenEVSE_Shaper) != EvseState::Disabled)
				{
					props.setState(EvseState::Disabled);
					evse.claim(EvseClient_OpenEVSE_Shaper, EvseManager_Priority_Limit, props);
					_awaiting_fresh_reading = true;
					_last_claimed_cur = 0;
					_last_change_time = millis();
					StaticJsonDocument<192> event;
					event["shaper"] = 1;
					event["shaper_live_pwr"] = _live_pwr;
					event["shaper_smoothed_live_pwr"] = _smoothed_live_pwr;
					event["shaper_safety_live_pwr"] = _safety_live_pwr;
					event["shaper_max_pwr"] = _max_pwr;
					event["shaper_cur"] = _max_cur;
					event["shaper_updated"] = _updated;
					event_send(event);
				}
			}
	}
	else {
		//remove shaper claim
		if (_evse->clientHasClaim(EvseClient_OpenEVSE_Shaper)) {
			_evse->release(EvseClient_OpenEVSE_Shaper);
			_smoothed_live_pwr = 0;
		}
	}

	return EVSE_SHAPER_LOOP_TIME;
}

void CurrentShaperTask::begin(EvseManager &evse) {
	this -> _timer   = millis();
	this -> _enabled = config_current_shaper_enabled();
	this -> _evse    = &evse;
	this -> _max_pwr = current_shaper_max_pwr;
	this -> _live_pwr = 0;
	this -> _smoothed_live_pwr = 0;
	this -> _max_cur = 0;
	this -> _updated = false;
	this -> _awaiting_fresh_reading = false;
	this -> _last_claimed_cur = 0;
	this -> _last_change_time = 0;
	this -> _safety_live_pwr = 0;
	this -> _safety_filter_seeded = false;
	this -> _safety_max_cur = 0;
	MicroTask.startTask(this);
	StaticJsonDocument<128> event;
	event["shaper"]  = 1;
	event_send(event);
}

void CurrentShaperTask::notifyConfigChanged( bool enabled, uint32_t max_pwr) {
	DBUGF("CurrentShaper: got config changed");
	_enabled = enabled;
	_max_pwr = max_pwr;
	if (!enabled) evse.release(EvseClient_OpenEVSE_Shaper);
	StaticJsonDocument<128> event;
	event["shaper"] = enabled == true ? 1 : 0;
	event["shaper_max_pwr"] = max_pwr;
	event_send(event);
}

void CurrentShaperTask::setMaxPwr(int max_pwr) {
		_max_pwr = max_pwr;
		shapeCurrent();
}

void CurrentShaperTask::setLivePwr(int live_pwr) {
	_live_pwr = live_pwr;
	// A genuinely new reading has arrived, so it's now safe to act on the
	// next calculation - it reflects conditions after our last change.
	_awaiting_fresh_reading = false;
	shapeCurrent();
}

// temporary change Current Shaper state without changing configuration
void CurrentShaperTask::setState(bool state) {
	_enabled = state;
	if (!_enabled) {
		//remove claim
		evse.release(EvseClient_OpenEVSE_Shaper);
	}
	StaticJsonDocument<128> event;
	event["shaper"]  = state?1:0;
	event_send(event);
}

double CurrentShaperTask::calcMaxCur(double livepwr) {
	int max_pwr = _max_pwr;

	if (config_divert_enabled() == true) {
		if ( divert_type == DIVERT_TYPE_SOLAR ) {
			max_pwr += solar;
		}
	}

	if(!config_threephase_enabled()) {
		return ((max_pwr - livepwr) / evse.getVoltage()) + evse.getAmps();
	}

	return ((max_pwr - livepwr) / evse.getVoltage() / 3.0) + evse.getAmps();
}

void CurrentShaperTask::shapeCurrent() {
	_updated = true;

	int livepwr;
	DBUGVAR(_pause_timer);
	if (_pause_timer == 0) {
		_smoothed_live_pwr = _live_pwr;
		livepwr = _live_pwr;
	}
	else {
		if (_live_pwr > _smoothed_live_pwr) {
			_smoothed_live_pwr = _live_pwr;
		}
		else {
			_smoothed_live_pwr = _inputFilter.filter(_live_pwr, _smoothed_live_pwr, current_shaper_smoothing_time);
		}
		livepwr = _smoothed_live_pwr;
	}

	_max_cur = calcMaxCur(livepwr);

	// Independent, always-on short-tau smoothing used only for the
	// pause/over-budget entry decision in loop() - so a multi-second
	// transient spike (e.g. an EV's precharge/self-test pulse) can't slam
	// the charge into pause on a single raw sample. Runs regardless of
	// _pause_timer, unlike _smoothed_live_pwr above, since the entry
	// decision this protects happens precisely while not yet paused.
	if (!_safety_filter_seeded) {
		// Seed to the raw reading (not filtered) so a genuine overload
		// already present on the very first-ever reading isn't masked.
		_safety_live_pwr = _live_pwr;
		_safety_filter_seeded = true;
	}
	else {
		_safety_live_pwr = _safetyFilter.filter(_live_pwr, _safety_live_pwr, EVSE_SHAPER_SAFETY_FILTER_TAU);
	}
	_safety_max_cur = calcMaxCur(_safety_live_pwr);

	_changed = true;
}

int CurrentShaperTask::getMaxPwr() {
	return _max_pwr;
}
int CurrentShaperTask::getLivePwr() {
	return _live_pwr;
}
int CurrentShaperTask::getSmoothedLivePwr() {
	return _smoothed_live_pwr;
}

double CurrentShaperTask::getMaxCur() {
	return _max_cur;
}
bool CurrentShaperTask::getState() {
	return _enabled;
}

bool CurrentShaperTask::isActive() {
	return _evse->clientHasClaim(EvseClient_OpenEVSE_Shaper);
}

bool CurrentShaperTask::isUpdated() {
	return _updated;
}
