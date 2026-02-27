#if defined(ENABLE_DEBUG) && !defined(ENABLE_DEBUG_LIMIT)
#undef ENABLE_DEBUG
#endif

#include "limit.h"
#include "debug.h"
#include "event.h"

// ---------------------------------------------
//             LimitType Class
// ---------------------------------------------

uint8_t LimitType::fromString(const char *value)
{
  switch (value[0]) {
    case ('n'): _value = LimitType::None; break;
    case ('t'): _value = LimitType::Time; break;
    case ('e'): _value = LimitType::Energy; break;
    case ('s'): _value = LimitType::Soc; break;
    case ('r'): _value = LimitType::Range; break;
  }
  return _value;
}

const char *LimitType::toString()
{
  return LimitType::None == _value ? "none" :
         LimitType::Time == _value ? "time" :
         LimitType::Energy == _value ? "energy" :
         LimitType::Soc == _value ? "soc" :
         LimitType::Range == _value ? "range" :
         "none";
}

LimitType LimitType::operator= (const Value val) {
  _value = val;
  return *this;
}

// ---------------------------------------------
//             LimitProperties Class
// ---------------------------------------------
LimitProperties::LimitProperties()
{
  _type = LimitType::None;
  _value = 0;
  _auto_release = true;
}

LimitProperties::~LimitProperties() {}

void LimitProperties::init()
{
  _type = LimitType::None;
  _value = 0;
  _auto_release = true;
}

LimitType LimitProperties::getType() { return _type; }

bool LimitProperties::setType(LimitType type)
{
  _type = type;
  return true;
}

uint32_t LimitProperties::getValue() { return _value; }

bool LimitProperties::setValue(uint32_t value)
{
  _value = value;
  return true;
}

bool LimitProperties::setAutoRelease(bool val) {
  _auto_release = val;
  return true;
}

bool LimitProperties::getAutoRelease() { return _auto_release; }

bool LimitProperties::deserialize(JsonObject &obj)
{
  if(obj.containsKey("type")) {
    _type.fromString(obj["type"]);
  }
  if(obj.containsKey("value")) {
    _value = obj["value"];
  }
  if(obj.containsKey("auto_release")) {
    _auto_release = obj["auto_release"];
  }
  return _type > 0 && _value > 0;
}

bool LimitProperties::serialize(JsonObject &obj)
{
  obj["type"] = _type.toString();
  obj["value"] = _value;
  obj["auto_release"] = _auto_release;
  return true;
}

// ---------------------------------------------
//             Limit Class
// ---------------------------------------------

Limit limit;

Limit::Limit() :
  MicroTasks::Task(),
  _version(0),
  _sessionCompleteListener(this)
{
  _limit_properties.init();
}

Limit::~Limit() {
  if (_evse) {
     _evse->release(EvseClient_OpenEVSE_Limit);
  }
}

void Limit::setup() {}

void Limit::begin(EvseManager &evse) {
  this->_evse = &evse;
  setDefaultLimit(limit_default_type.c_str(), limit_default_value);
  MicroTask.startTask(this);
  _evse->onSessionComplete(&_sessionCompleteListener);
}

unsigned long Limit::loop(MicroTasks::WakeReason reason)
{
  if (!_evse) return EVSE_LIMIT_LOOP_TIME;

  if(_sessionCompleteListener.IsTriggered())
  {
    if (_evse->clientHasClaim(EvseClient_OpenEVSE_Limit)) {
      _evse->release(EvseClient_OpenEVSE_Limit);
    }
    if (_limit_properties.getAutoRelease()){
      clear();
    }
  }

  if (hasLimit())
  {
    LimitType type = _limit_properties.getType();
    uint32_t value = _limit_properties.getValue();
    
    if(_evse->isCharging())
    {
      bool limit_reached = false;
      switch (type) {
        case LimitType::Time: limit_reached = limitTime(value); break;
        case LimitType::Energy: limit_reached = limitEnergy(value); break;
        case LimitType::Soc: limit_reached = limitSoc(value); break;
        case LimitType::Range: limit_reached = limitRange(value); break;
        default: break;
      }

      if (limit_reached) {
        EvseProperties props;
        props.setState(EvseState::Disabled);
        props.setAutoRelease(true);
        _evse->claim(EvseClient_OpenEVSE_Limit, EvseManager_Priority_Limit, props);
      }
    }
    // Lógica dinámica para respetar el Schedule
    else if(_limit_properties.getAutoRelease())
    {
      bool schedule_blocks_charge = false;

      // Consultamos si el Schedule está activo ahora mismo
      if (_evse->clientHasClaim(EvseClient_OpenEVSE_Schedule)) 
      {
          EvseState schedState = _evse->getClaimProperties(EvseClient_OpenEVSE_Schedule).getState();
          if (schedState != EvseState::Active) {
              schedule_blocks_charge = true;
          }
      }

      if (schedule_blocks_charge) {
          // Si el horario bloquea, liberamos el límite para que el cargador duerma
          if (_evse->clientHasClaim(EvseClient_OpenEVSE_Limit)) {
              _evse->release(EvseClient_OpenEVSE_Limit);
          }
      } 
      else if (EvseState::Disabled == config_default_state() && !_evse->clientHasClaim(EvseClient_OpenEVSE_Limit)) 
      {
          // Si el horario permite (o no hay) y el estado base es Disabled, activamos carga
          EvseProperties props;
          props.setState(EvseState::Active);
          props.setAutoRelease(true);
          _evse->claim(EvseClient_OpenEVSE_Limit, EvseManager_Priority_Limit, props);
      }
    }
  }
  else
  {
    if (_evse->clientHasClaim(EvseClient_OpenEVSE_Limit)) {
      _evse->release(EvseClient_OpenEVSE_Limit);
    }
  }
  return EVSE_LIMIT_LOOP_TIME;
}

bool Limit::limitTime(uint32_t val) {
  uint32_t elapsed = (uint32_t)_evse->getSessionElapsed()/60;
  return (val > 0 && elapsed >= val);
}

bool Limit::limitEnergy(uint32_t val) {
  uint32_t elapsed = _evse->getSessionEnergy();
  return (val > 0 && elapsed >= val);
}

bool Limit::limitSoc(uint32_t val) {
  uint32_t soc = _evse->getVehicleStateOfCharge();
  return (val > 0 && soc >= val);
}

bool Limit::limitRange(uint32_t val) {
  uint32_t rng = _evse->getVehicleRange();
  return (val > 0 && rng >= val);
}

bool Limit::hasLimit() { return _limit_properties.getType() != LimitType::None; }

bool Limit::set(String json) {
  LimitProperties props;
  if (props.deserialize(json)) {
    set(props);
    return true;
  }
  return false;
}

bool Limit::set(LimitProperties props) {
  _limit_properties = props;
  StaticJsonDocument<32> doc;
  doc["limit"] = hasLimit();
  doc["limit_version"] = ++_version;
  event_send(doc);
  return true;
}

LimitProperties Limit::get() { return _limit_properties; }

bool Limit::clear() {
  _limit_properties.init();
  StaticJsonDocument<32> doc;
  doc["limit"] = false;
  doc["limit_version"] = ++_version;
  event_send(doc);
  return true;
}

uint8_t Limit::getVersion() { return _version; }

bool Limit::setDefaultLimit(const char* typeStr, uint32_t value) {
  LimitProperties limitprops;
  LimitType limitType;
  limitType.fromString(typeStr);
  limitprops.setType(limitType);
  limitprops.setValue(value);
  limitprops.setAutoRelease(false);

  if (limitType == LimitType::None) {
    clear();
    return true;
  } else if (limitprops.getValue()) {
    set(limitprops);
    return true;
  }
  return false;
}
