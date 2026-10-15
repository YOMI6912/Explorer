




#include "VL53L1X.h"



VL53L1X::VL53L1X()
#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_TWOWIRE)
  : bus(&Wire)
#else
  : bus(nullptr)
#endif
  , address(AddressDefault)
  , io_timeout(0)
  , did_timeout(false)
  , io_error(false)
  , last_read_i2c_retries(0)
  , calibrated(false)
  , saved_vhv_init(0)
  , saved_vhv_timeout(0)
  , distance_mode(Unknown)
{
}



void VL53L1X::setAddress(uint8_t new_addr)
{
  writeReg(I2C_SLAVE__DEVICE_ADDRESS, new_addr & 0x7F);
  address = new_addr;
}





bool VL53L1X::init(bool io_2v8)
{



  calibrated = false;
  saved_vhv_init = 0;
  saved_vhv_timeout = 0;
  did_timeout = false;
  last_read_i2c_retries = 0;
  io_error = false;

  if (readReg16Bit(IDENTIFICATION__MODEL_ID) != 0xEACC) { return false; }






  delay(20);



  startTimeout();


  while ((readReg(FIRMWARE__SYSTEM_STATUS) & 0x01) == 0 || last_status != 0)
  {
    if (checkTimeoutExpired())
    {
      did_timeout = true;
      return false;
    }
  }





  if (io_2v8)
  {
    writeReg(PAD_I2C_HV__EXTSUP_CONFIG,
      readReg(PAD_I2C_HV__EXTSUP_CONFIG) | 0x01);
  }


  fast_osc_frequency = readReg16Bit(OSC_MEASURED__FAST_OSC__FREQUENCY);
  osc_calibrate_val = readReg16Bit(RESULT__OSC_CALIBRATE_VAL);






  if (last_status != 0 || fast_osc_frequency == 0)
  {
    return false;
  }






















  writeReg16Bit(DSS_CONFIG__TARGET_TOTAL_RATE_MCPS, TargetRate);
  writeReg(GPIO__TIO_HV_STATUS, 0x02);
  writeReg(SIGMA_ESTIMATOR__EFFECTIVE_PULSE_WIDTH_NS, 8);
  writeReg(SIGMA_ESTIMATOR__EFFECTIVE_AMBIENT_WIDTH_NS, 16);
  writeReg(ALGO__CROSSTALK_COMPENSATION_VALID_HEIGHT_MM, 0x01);
  writeReg(ALGO__RANGE_IGNORE_VALID_HEIGHT_MM, 0xFF);
  writeReg(ALGO__RANGE_MIN_CLIP, 0);
  writeReg(ALGO__CONSISTENCY_CHECK__TOLERANCE, 2);


  writeReg16Bit(SYSTEM__THRESH_RATE_HIGH, 0x0000);
  writeReg16Bit(SYSTEM__THRESH_RATE_LOW, 0x0000);
  writeReg(DSS_CONFIG__APERTURE_ATTENUATION, 0x38);




  writeReg16Bit(RANGE_CONFIG__SIGMA_THRESH, 360);
  writeReg16Bit(RANGE_CONFIG__MIN_COUNT_RATE_RTN_LIMIT_MCPS, 192);



  writeReg(SYSTEM__GROUPED_PARAMETER_HOLD_0, 0x01);
  writeReg(SYSTEM__GROUPED_PARAMETER_HOLD_1, 0x01);
  writeReg(SD_CONFIG__QUANTIFIER, 2);







  writeReg(SYSTEM__GROUPED_PARAMETER_HOLD, 0x00);
  writeReg(SYSTEM__SEED_CONFIG, 1);


  writeReg(SYSTEM__SEQUENCE_CONFIG, 0x8B);
  writeReg16Bit(DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, 200 << 8);
  writeReg(DSS_CONFIG__ROI_MODE_CONTROL, 2);





  setDistanceMode(Long);
  setMeasurementTimingBudget(50000);





  writeReg16Bit(ALGO__PART_TO_PART_RANGE_OFFSET_MM,
    readReg16Bit(MM_CONFIG__OUTER_OFFSET_MM) * 4);

  return !io_error && last_status == 0;
}


void VL53L1X::writeReg(uint16_t reg, uint8_t value)
{
  for (uint8_t attempt = 0; attempt < 3; ++attempt)
  {
    bus->beginTransmission(address);
    bus->write((uint8_t)(reg >> 8));
    bus->write((uint8_t)(reg));
    bus->write(value);
    last_status = bus->endTransmission();
    if (last_status == 0) { return; }
    if (last_read_i2c_retries < UINT16_MAX) { ++last_read_i2c_retries; }
    delay(2);
  }
  io_error = true;
}


void VL53L1X::writeReg16Bit(uint16_t reg, uint16_t value)
{
  for (uint8_t attempt = 0; attempt < 3; ++attempt)
  {
    bus->beginTransmission(address);
    bus->write((uint8_t)(reg >> 8));
    bus->write((uint8_t)(reg));
    bus->write((uint8_t)(value >> 8));
    bus->write((uint8_t)(value));
    last_status = bus->endTransmission();
    if (last_status == 0) { return; }
    if (last_read_i2c_retries < UINT16_MAX) { ++last_read_i2c_retries; }
    delay(2);
  }
  io_error = true;
}


void VL53L1X::writeReg32Bit(uint16_t reg, uint32_t value)
{
  for (uint8_t attempt = 0; attempt < 3; ++attempt)
  {
    bus->beginTransmission(address);
    bus->write((uint8_t)(reg >> 8));
    bus->write((uint8_t)(reg));
    bus->write((uint8_t)(value >> 24));
    bus->write((uint8_t)(value >> 16));
    bus->write((uint8_t)(value >>  8));
    bus->write((uint8_t)(value));
    last_status = bus->endTransmission();
    if (last_status == 0) { return; }
    if (last_read_i2c_retries < UINT16_MAX) { ++last_read_i2c_retries; }
    delay(2);
  }
  io_error = true;
}


uint8_t VL53L1X::readReg(regAddr reg)
{
  for (uint8_t attempt = 0; attempt < 3; ++attempt)
  {
    bus->beginTransmission(address);
    bus->write((uint8_t)(reg >> 8));
    bus->write((uint8_t)(reg));
    last_status = bus->endTransmission();
    if (last_status == 0 && bus->requestFrom(address, (uint8_t)1) == 1 &&
        bus->available() >= 1)
    {
      last_status = 0;
      return (uint8_t)bus->read();
    }
    while (bus->available()) { bus->read(); }
    if (last_status == 0) { last_status = 5; }
    if (last_read_i2c_retries < UINT16_MAX) { ++last_read_i2c_retries; }
    delay(2);
  }
  io_error = true;
  return 0;
}


uint16_t VL53L1X::readReg16Bit(uint16_t reg)
{
  for (uint8_t attempt = 0; attempt < 3; ++attempt)
  {
    bus->beginTransmission(address);
    bus->write((uint8_t)(reg >> 8));
    bus->write((uint8_t)(reg));
    last_status = bus->endTransmission();
    if (last_status == 0 && bus->requestFrom(address, (uint8_t)2) == 2 &&
        bus->available() >= 2)
    {
      uint16_t value  = (uint16_t)bus->read() << 8;
      value |= (uint8_t)bus->read();
      last_status = 0;
      return value;
    }
    while (bus->available()) { bus->read(); }
    if (last_status == 0) { last_status = 5; }
    if (last_read_i2c_retries < UINT16_MAX) { ++last_read_i2c_retries; }
    delay(2);
  }
  io_error = true;
  return 0;
}


uint32_t VL53L1X::readReg32Bit(uint16_t reg)
{
  for (uint8_t attempt = 0; attempt < 3; ++attempt)
  {
    bus->beginTransmission(address);
    bus->write((uint8_t)(reg >> 8));
    bus->write((uint8_t)(reg));
    last_status = bus->endTransmission();
    if (last_status == 0 && bus->requestFrom(address, (uint8_t)4) == 4 &&
        bus->available() >= 4)
    {
      uint32_t value  = (uint32_t)bus->read() << 24;
      value |= (uint32_t)bus->read() << 16;
      value |= (uint16_t)bus->read() << 8;
      value |= (uint8_t)bus->read();
      last_status = 0;
      return value;
    }
    while (bus->available()) { bus->read(); }
    if (last_status == 0) { last_status = 5; }
    if (last_read_i2c_retries < UINT16_MAX) { ++last_read_i2c_retries; }
    delay(2);
  }
  io_error = true;
  return 0;
}



bool VL53L1X::setDistanceMode(DistanceMode mode)
{

  uint32_t budget_us = getMeasurementTimingBudget();

  switch (mode)
  {
    case Short:



      writeReg(RANGE_CONFIG__VCSEL_PERIOD_A, 0x07);
      writeReg(RANGE_CONFIG__VCSEL_PERIOD_B, 0x05);
      writeReg(RANGE_CONFIG__VALID_PHASE_HIGH, 0x38);


      writeReg(SD_CONFIG__WOI_SD0, 0x07);
      writeReg(SD_CONFIG__WOI_SD1, 0x05);
      writeReg(SD_CONFIG__INITIAL_PHASE_SD0, 6);
      writeReg(SD_CONFIG__INITIAL_PHASE_SD1, 6);

      break;

    case Medium:



      writeReg(RANGE_CONFIG__VCSEL_PERIOD_A, 0x0B);
      writeReg(RANGE_CONFIG__VCSEL_PERIOD_B, 0x09);
      writeReg(RANGE_CONFIG__VALID_PHASE_HIGH, 0x78);


      writeReg(SD_CONFIG__WOI_SD0, 0x0B);
      writeReg(SD_CONFIG__WOI_SD1, 0x09);
      writeReg(SD_CONFIG__INITIAL_PHASE_SD0, 10);
      writeReg(SD_CONFIG__INITIAL_PHASE_SD1, 10);

      break;

    case Long:



      writeReg(RANGE_CONFIG__VCSEL_PERIOD_A, 0x0F);
      writeReg(RANGE_CONFIG__VCSEL_PERIOD_B, 0x0D);
      writeReg(RANGE_CONFIG__VALID_PHASE_HIGH, 0xB8);


      writeReg(SD_CONFIG__WOI_SD0, 0x0F);
      writeReg(SD_CONFIG__WOI_SD1, 0x0D);
      writeReg(SD_CONFIG__INITIAL_PHASE_SD0, 14);
      writeReg(SD_CONFIG__INITIAL_PHASE_SD1, 14);

      break;

    default:

      return false;
  }


  setMeasurementTimingBudget(budget_us);


  distance_mode = mode;

  return true;
}





bool VL53L1X::setMeasurementTimingBudget(uint32_t budget_us)
{


  if (budget_us <= TimingGuard) { return false; }

  uint32_t range_config_timeout_us = budget_us -= TimingGuard;
  if (range_config_timeout_us > 1100000) { return false; }

  range_config_timeout_us /= 2;



  uint32_t macro_period_us;


  macro_period_us = calcMacroPeriod(readReg(RANGE_CONFIG__VCSEL_PERIOD_A));




  uint32_t phasecal_timeout_mclks = timeoutMicrosecondsToMclks(1000, macro_period_us);
  if (phasecal_timeout_mclks > 0xFF) { phasecal_timeout_mclks = 0xFF; }
  writeReg(PHASECAL_CONFIG__TIMEOUT_MACROP, phasecal_timeout_mclks);








  writeReg16Bit(MM_CONFIG__TIMEOUT_MACROP_A, encodeTimeout(
    timeoutMicrosecondsToMclks(1, macro_period_us)));


  writeReg16Bit(RANGE_CONFIG__TIMEOUT_MACROP_A, encodeTimeout(
    timeoutMicrosecondsToMclks(range_config_timeout_us, macro_period_us)));


  macro_period_us = calcMacroPeriod(readReg(RANGE_CONFIG__VCSEL_PERIOD_B));



  writeReg16Bit(MM_CONFIG__TIMEOUT_MACROP_B, encodeTimeout(
    timeoutMicrosecondsToMclks(1, macro_period_us)));


  writeReg16Bit(RANGE_CONFIG__TIMEOUT_MACROP_B, encodeTimeout(
    timeoutMicrosecondsToMclks(range_config_timeout_us, macro_period_us)));



  return true;
}



uint32_t VL53L1X::getMeasurementTimingBudget()
{






  uint32_t macro_period_us = calcMacroPeriod(readReg(RANGE_CONFIG__VCSEL_PERIOD_A));



  uint32_t range_config_timeout_us = timeoutMclksToMicroseconds(decodeTimeout(
    readReg16Bit(RANGE_CONFIG__TIMEOUT_MACROP_A)), macro_period_us);



  return  2 * range_config_timeout_us + TimingGuard;
}






void VL53L1X::setROISize(uint8_t width, uint8_t height)
{
  if ( width > 16) {  width = 16; }
  if (height > 16) { height = 16; }




  if (width > 10 || height > 10)
  {
    writeReg(ROI_CONFIG__USER_ROI_CENTRE_SPAD, 199);
  }

  writeReg(ROI_CONFIG__USER_ROI_REQUESTED_GLOBAL_XY_SIZE,
           (height - 1) << 4 | (width - 1));
}



void VL53L1X::getROISize(uint8_t * width, uint8_t * height)
{
  uint8_t reg_val = readReg(ROI_CONFIG__USER_ROI_REQUESTED_GLOBAL_XY_SIZE);
  *width = (reg_val & 0xF) + 1;
  *height = (reg_val >> 4) + 1;
}


















































void VL53L1X::setROICenter(uint8_t spadNumber)
{
  writeReg(ROI_CONFIG__USER_ROI_CENTRE_SPAD, spadNumber);
}



uint8_t VL53L1X::getROICenter()
{
  return readReg(ROI_CONFIG__USER_ROI_CENTRE_SPAD);
}



void VL53L1X::startContinuous(uint32_t period_ms)
{

  writeReg32Bit(SYSTEM__INTERMEASUREMENT_PERIOD, period_ms * osc_calibrate_val);

  writeReg(SYSTEM__INTERRUPT_CLEAR, 0x01);
  writeReg(SYSTEM__MODE_START, 0x40);
}



void VL53L1X::stopContinuous()
{
  writeReg(SYSTEM__MODE_START, 0x80);



  calibrated = false;


  if (saved_vhv_init != 0)
  {
    writeReg(VHV_CONFIG__INIT, saved_vhv_init);
  }
  if (saved_vhv_timeout != 0)
  {
     writeReg(VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND, saved_vhv_timeout);
  }


  writeReg(PHASECAL_CONFIG__OVERRIDE, 0x00);


}






uint16_t VL53L1X::read(bool blocking)
{
  io_error = false;
  last_read_i2c_retries = 0;
  auto invalidReading = [this]() -> uint16_t {
    ranging_data.range_mm = 0;
    ranging_data.range_status = None;
    return 0;
  };

  if (blocking)
  {
    startTimeout();
    while (!dataReady())
    {
      if (io_error) { return invalidReading(); }
      if (checkTimeoutExpired())
      {
        did_timeout = true;
        return invalidReading();
      }
      delay(2);
    }
  }

  readResults();
  if (io_error) { return invalidReading(); }

  if (!calibrated)
  {
    setupManualCalibration();
    if (io_error) { return invalidReading(); }
    calibrated = true;
  }

  updateDSS();
  if (io_error) { return invalidReading(); }

  getRangingData();

  writeReg(SYSTEM__INTERRUPT_CLEAR, 0x01);
  if (io_error) { return invalidReading(); }

  return ranging_data.range_mm;
}




uint16_t VL53L1X::readSingle(bool blocking)
{
  writeReg(SYSTEM__INTERRUPT_CLEAR, 0x01);
  writeReg(SYSTEM__MODE_START, 0x10);

  if (blocking)
  {
    return read(true);
  }
  else
  {
    return 0;
  }
}






const char * VL53L1X::rangeStatusToString(RangeStatus status)
{
  switch (status)
  {
    case RangeValid:
      return "range valid";

    case SigmaFail:
      return "sigma fail";

    case SignalFail:
      return "signal fail";

    case RangeValidMinRangeClipped:
      return "range valid, min range clipped";

    case OutOfBoundsFail:
      return "out of bounds fail";

    case HardwareFail:
      return "hardware fail";

    case RangeValidNoWrapCheckFail:
      return "range valid, no wrap check fail";

    case WrapTargetFail:
      return "wrap target fail";

    case XtalkSignalFail:
      return "xtalk signal fail";

    case SynchronizationInt:
      return "synchronization int";

    case MinRangeFail:
      return "min range fail";

    case None:
      return "no update";

    default:
      return "unknown status";
  }
}



bool VL53L1X::timeoutOccurred()
{
  bool tmp = did_timeout;
  did_timeout = false;
  return tmp;
}






void VL53L1X::setupManualCalibration()
{

  saved_vhv_init = readReg(VHV_CONFIG__INIT);
  saved_vhv_timeout = readReg(VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND);


  writeReg(VHV_CONFIG__INIT, saved_vhv_init & 0x7F);


  writeReg(VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND,
    (saved_vhv_timeout & 0x03) + (3 << 2));


  writeReg(PHASECAL_CONFIG__OVERRIDE, 0x01);
  writeReg(CAL_CONFIG__VCSEL_START, readReg(PHASECAL_RESULT__VCSEL_START));
}


void VL53L1X::readResults()
{
  bool received = false;
  for (uint8_t attempt = 0; attempt < 3; ++attempt)
  {
    bus->beginTransmission(address);
    bus->write((uint8_t)(RESULT__RANGE_STATUS >> 8));
    bus->write((uint8_t)(RESULT__RANGE_STATUS));
    last_status = bus->endTransmission();
    if (last_status == 0 && bus->requestFrom(address, (uint8_t)17) == 17 &&
        bus->available() >= 17)
    {
      received = true;
      last_status = 0;
      break;
    }
    while (bus->available()) { bus->read(); }
    if (last_status == 0) { last_status = 5; }
    if (last_read_i2c_retries < UINT16_MAX) { ++last_read_i2c_retries; }
    delay(2);
  }

  if (!received)
  {
    io_error = true;
    results.range_status = 0xFF;
    results.stream_count = 0;
    results.dss_actual_effective_spads_sd0 = 0;
    results.ambient_count_rate_mcps_sd0 = 0;
    results.final_crosstalk_corrected_range_mm_sd0 = 0;
    results.peak_signal_count_rate_crosstalk_corrected_mcps_sd0 = 0;
    return;
  }

  results.range_status = bus->read();

  bus->read();

  results.stream_count = bus->read();

  results.dss_actual_effective_spads_sd0  = (uint16_t)bus->read() << 8;
  results.dss_actual_effective_spads_sd0 |=           bus->read();

  bus->read();
  bus->read();

  results.ambient_count_rate_mcps_sd0  = (uint16_t)bus->read() << 8;
  results.ambient_count_rate_mcps_sd0 |=           bus->read();

  bus->read();
  bus->read();

  bus->read();
  bus->read();

  results.final_crosstalk_corrected_range_mm_sd0  = (uint16_t)bus->read() << 8;
  results.final_crosstalk_corrected_range_mm_sd0 |=           bus->read();

  results.peak_signal_count_rate_crosstalk_corrected_mcps_sd0  = (uint16_t)bus->read() << 8;
  results.peak_signal_count_rate_crosstalk_corrected_mcps_sd0 |=           bus->read();
}



void VL53L1X::updateDSS()
{
  uint16_t spadCount = results.dss_actual_effective_spads_sd0;

  if (spadCount != 0)
  {


    uint32_t totalRatePerSpad =
      (uint32_t)results.peak_signal_count_rate_crosstalk_corrected_mcps_sd0 +
      results.ambient_count_rate_mcps_sd0;


    if (totalRatePerSpad > 0xFFFF) { totalRatePerSpad = 0xFFFF; }


    totalRatePerSpad <<= 16;

    totalRatePerSpad /= spadCount;

    if (totalRatePerSpad != 0)
    {

      uint32_t requiredSpads = ((uint32_t)TargetRate << 16) / totalRatePerSpad;


      if (requiredSpads > 0xFFFF) { requiredSpads = 0xFFFF; }


      writeReg16Bit(DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, requiredSpads);


      return;
    }
  }






   writeReg16Bit(DSS_CONFIG__MANUAL_EFFECTIVE_SPADS_SELECT, 0x8000);
}



void VL53L1X::getRangingData()
{


  uint16_t range = results.final_crosstalk_corrected_range_mm_sd0;





  ranging_data.range_mm = ((uint32_t)range * 2011 + 0x0400) / 0x0800;





  switch(results.range_status)
  {
    case 17:
    case 2:
    case 1:
    case 3:

      ranging_data.range_status = HardwareFail;
      break;

    case 13:

      ranging_data.range_status = MinRangeFail;
      break;

    case 18:
      ranging_data.range_status = SynchronizationInt;
      break;

    case 5:
      ranging_data.range_status =  OutOfBoundsFail;
      break;

    case 4:
      ranging_data.range_status = SignalFail;
      break;

    case 6:
      ranging_data.range_status = SigmaFail;
      break;

    case 7:
      ranging_data.range_status = WrapTargetFail;
      break;

    case 12:
      ranging_data.range_status = XtalkSignalFail;
      break;

    case 8:
      ranging_data.range_status = RangeValidMinRangeClipped;
      break;

    case 9:

      if (results.stream_count == 0)
      {
        ranging_data.range_status = RangeValidNoWrapCheckFail;
      }
      else
      {
        ranging_data.range_status = RangeValid;
      }
      break;

    default:
      ranging_data.range_status = None;
  }


  ranging_data.peak_signal_count_rate_MCPS =
    countRateFixedToFloat(results.peak_signal_count_rate_crosstalk_corrected_mcps_sd0);
  ranging_data.ambient_count_rate_MCPS =
    countRateFixedToFloat(results.ambient_count_rate_mcps_sd0);
}



uint32_t VL53L1X::decodeTimeout(uint16_t reg_val)
{
  return ((uint32_t)(reg_val & 0xFF) << (reg_val >> 8)) + 1;
}



uint16_t VL53L1X::encodeTimeout(uint32_t timeout_mclks)
{


  uint32_t ls_byte = 0;
  uint16_t ms_byte = 0;

  if (timeout_mclks > 0)
  {
    ls_byte = timeout_mclks - 1;

    while ((ls_byte & 0xFFFFFF00) > 0)
    {
      ls_byte >>= 1;
      ms_byte++;
    }

    return (ms_byte << 8) | (ls_byte & 0xFF);
  }
  else { return 0; }
}




uint32_t VL53L1X::timeoutMclksToMicroseconds(uint32_t timeout_mclks, uint32_t macro_period_us)
{
  return ((uint64_t)timeout_mclks * macro_period_us + 0x800) >> 12;
}




uint32_t VL53L1X::timeoutMicrosecondsToMclks(uint32_t timeout_us, uint32_t macro_period_us)
{
  if (macro_period_us == 0) { return 0; }
  return (((uint32_t)timeout_us << 12) + (macro_period_us >> 1)) / macro_period_us;
}




uint32_t VL53L1X::calcMacroPeriod(uint8_t vcsel_period)
{
  if (fast_osc_frequency == 0) { return 0; }



  uint32_t pll_period_us = ((uint32_t)0x01 << 30) / fast_osc_frequency;


  uint8_t vcsel_period_pclks = (vcsel_period + 1) << 1;


  uint32_t macro_period_us = (uint32_t)2304 * pll_period_us;
  macro_period_us >>= 6;
  macro_period_us *= vcsel_period_pclks;
  macro_period_us >>= 6;

  return macro_period_us;
}
