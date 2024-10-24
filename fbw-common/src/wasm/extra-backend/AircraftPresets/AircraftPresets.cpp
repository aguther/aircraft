// Copyright (c) 2023 FlyByWire Simulations
// SPDX-License-Identifier: GPL-3.0

#include <iostream>

#include "AircraftPresets.h"
#include "SimUnits.h"
#include "UpdateMode.h"
#include "logging.h"
#include "math_utils.hpp"

///
// DataManager Howto Note:
// =======================
//
// The AircraftPresets module uses the DataManager to get and set variables.
// Looking at the make_xxx_var functions, you can see that they are updated
// with different update cycles.
//
// Some variables are read from the sim at every tick:
// - A32NX_LOAD_AIRCRAFT_PRESET
// - SIM ON GROUND
//
// The rest are read on demand after the state of the above variables have been checked.
// No variable is written automatically.
//
// This makes sure variables are only read or written to when really needed. And as
// AircraftPresets will be dormant most of the time, this is saving a lot of
// unnecessary reads/writes.
//
// In addition, the AircraftPresets module is a very specific use case amd uses
// SimConnect execute_calculator_code extensively for the procedures to work.
// This is a good demonstration that the Cpp WASM framework does not limit
// applications to a specific pattern.
///

bool AircraftPresets::initialize() {
  dataManager = &msfsHandler.getDataManager();

  // LVARs
  aircraftPresetVerbose = dataManager->make_named_var("AIRCRAFT_PRESET_VERBOSE", UNITS.Bool, UpdateMode::AUTO_READ);
  loadAircraftPresetRequest = dataManager->make_named_var("AIRCRAFT_PRESET_LOAD", UNITS.Number, UpdateMode::AUTO_READ_WRITE);
  progressAircraftPreset = dataManager->make_named_var("AIRCRAFT_PRESET_LOAD_PROGRESS");
  progressAircraftPresetId = dataManager->make_named_var("AIRCRAFT_PRESET_LOAD_CURRENT_ID");
  loadAircraftPresetRequest->setAndWriteToSim(0);  // reset to 0 on startup

  // Simvars
  simOnGround = dataManager->make_simple_aircraft_var("SIM ON GROUND", UNITS.Number, true);

  _isInitialized = true;
  LOG_INFO("AircraftPresets initialized");
  return true;
}

bool AircraftPresets::update(sGaugeDrawData* pData) {
  if (!_isInitialized) {
    LOG_ERROR("AircraftPresets::update() - not initialized");
    return false;
  }

  if (!msfsHandler.getAircraftIsReadyVar())
    return true;

  // has request to load a preset been received?
  if (loadAircraftPresetRequest->getAsInt64() > 0) {
    // we do not allow loading of presets in the air to prevent users from
    // accidentally changing the aircraft configuration
    if (!simOnGround->getAsBool()) {
      LOG_WARN("AircraftPresets: Aircraft must be on the ground to load a preset!");
      loadAircraftPresetRequest->setAsInt64(0);
      loadingIsActive = false;
      return true;
    }

    // read the progress vars once to get the current state
    progressAircraftPreset->updateFromSim(msfsHandler.getTimeStamp(), msfsHandler.getTickCounter());
    progressAircraftPresetId->updateFromSim(msfsHandler.getTimeStamp(), msfsHandler.getTickCounter());

    // check if we already have an active loading process or if this is a new request which
    // needs to be initialized
    if (!loadingIsActive) {
      // get the requested procedure
      const std::optional<const Procedure*> requestedProcedure = presetProcedures.getProcedure(loadAircraftPresetRequest->getAsInt64());

      // check if procedure ID exists
      if (!requestedProcedure.has_value()) {
        LOG_WARN("AircraftPresets: Preset " + std::to_string(loadAircraftPresetRequest->getAsInt64()) + " not found!");
        loadAircraftPresetRequest->set(0);
        loadingIsActive = false;
        return true;
      }

      // initialize new loading process
      currentProcedureID = loadAircraftPresetRequest->getAsInt64();
      currentProcedure = requestedProcedure.value();
      currentLoadingTime = 0;
      currentDelay = 0;
      currentStep = 0;
      loadingIsActive = true;
      progressAircraftPreset->setAndWriteToSim(0);
      progressAircraftPresetId->setAndWriteToSim(0);
      LOG_INFO("AircraftPresets: Aircraft Preset " + std::to_string(currentProcedureID) + " starting procedure!");
      return true;
    }

    // reset the LVAR to the currently running procedure in case it has been changed
    // during a running procedure. We only allow "0" as a signal to interrupt the
    // current procedure
    loadAircraftPresetRequest->setAsInt64(currentProcedureID);

    // check if all procedure steps are done and the procedure is finished
    if (currentStep >= currentProcedure->size()) {
      LOG_INFO("AircraftPresets: Aircraft Preset " + std::to_string(currentProcedureID) + " done!");
      progressAircraftPreset->setAndWriteToSim(0);
      progressAircraftPresetId->setAndWriteToSim(0);
      loadAircraftPresetRequest->set(0);
      loadingIsActive = false;
      return true;
    }

    // update run timer
    currentLoadingTime += pData->dt * 1000;

    // check if we are in a delay and return if we have to wait
    if (currentLoadingTime <= currentDelay)
      return true;

    // convenience tmp
    const ProcedureStep* currentStepPtr = (*currentProcedure)[currentStep];

    // calculate next delay
    currentDelay = currentLoadingTime + currentStepPtr->delayAfter;

    // prepare return values for execute_calculator_code
    FLOAT64 fvalue = 0.0;
    SINT32 ivalue = 0;
    PCSTRINGZ svalue = nullptr;

    // check if the current step is a condition step and check the condition
    if (currentStepPtr->isConditional) {
      // update progress var
      progressAircraftPreset->setAndWriteToSim(static_cast<double>(currentStep) / currentProcedure->size());
      progressAircraftPresetId->setAndWriteToSim(currentStepPtr->id);
      execute_calculator_code(currentStepPtr->actionCode.c_str(), &fvalue, &ivalue, &svalue);
      LOG_INFO("AircraftPresets: Aircraft Preset Step " + std::to_string(currentStep) + " Condition: " + currentStepPtr->description +
               " (delay between tests: " + std::to_string(currentStepPtr->delayAfter) + ")");
      if (!helper::Math::almostEqual(0.0, fvalue)) {
        currentDelay = 0;
        currentStep++;
      }
      return true;
    }

    // test if the next step is required or if the state is already set in
    // which case the action can be skipped and delay can be ignored.
    fvalue = 0;
    ivalue = 0;
    svalue = nullptr;
    if (!currentStepPtr->expectedStateCheckCode.empty()) {
      if (aircraftPresetVerbose->getAsBool()) {
        std::cout << "AircraftPresets: Aircraft Preset Step " << currentStep << " Test: " << currentStepPtr->description << " TEST: \""
                  << currentStepPtr->expectedStateCheckCode << "\"" << std::endl;
      }
      execute_calculator_code(currentStepPtr->expectedStateCheckCode.c_str(), &fvalue, &ivalue, &svalue);
      if (!helper::Math::almostEqual(0.0, fvalue)) {
        if (aircraftPresetVerbose->getAsBool()) {
          std::cout << "AircraftPresets: Aircraft Preset Step " << currentStep << " Skipping: " << currentStepPtr->description
                    << " TEST: \"" << currentStepPtr->expectedStateCheckCode << "\"" << std::endl;
        }
        currentDelay = 0;
        currentStep++;
        return true;
      }
    }

    // update progress var
    progressAircraftPreset->setAndWriteToSim(static_cast<double>(currentStep) / currentProcedure->size());
    progressAircraftPresetId->setAndWriteToSim(currentStepPtr->id);

    // execute code to set expected state
    LOG_INFO("AircraftPresets: Aircraft Preset Step " + std::to_string(currentStep) + " Execute: " + currentStepPtr->description +
             " (delay after: " + std::to_string(currentStepPtr->delayAfter) + ")");
    execute_calculator_code(currentStepPtr->actionCode.c_str(), &fvalue, &ivalue, &svalue);
    currentStep++;

  } else if (loadingIsActive) {
    // request lvar has been set to 0 while we were executing a procedure ==> cancel loading
    LOG_INFO("AircraftPresets:update() Aircraft Preset " + std::to_string(currentProcedureID) + " loading cancelled!");
    loadingIsActive = false;
  }

  return true;
}

bool AircraftPresets::shutdown() {
  _isInitialized = false;
  std::cout << "AircraftPresets::shutdown()" << std::endl;
  return true;
}
