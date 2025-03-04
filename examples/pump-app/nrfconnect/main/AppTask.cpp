/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "AppTask.h"
#include "AppConfig.h"
#include "LEDWidget.h"
#include "PumpManager.h"
#include "ThreadUtil.h"
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>

#include <app-common/zap-generated/attribute-id.h>
#include <app-common/zap-generated/attribute-type.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-id.h>
#include <app/util/attribute-storage.h>

#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>

#include <platform/CHIPDeviceLayer.h>

#include <dk_buttons_and_leds.h>
#include <logging/log.h>
#include <zephyr.h>

#define FACTORY_RESET_TRIGGER_TIMEOUT 3000
#define FACTORY_RESET_CANCEL_WINDOW_TIMEOUT 3000
#define APP_EVENT_QUEUE_SIZE 10
#define BUTTON_PUSH_EVENT 1
#define BUTTON_RELEASE_EVENT 0

#define PCC_CLUSTER_ENDPOINT 1
#define ONOFF_CLUSTER_ENDPOINT 1

LOG_MODULE_DECLARE(app);
K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), APP_EVENT_QUEUE_SIZE, alignof(AppEvent));

static LEDWidget sStatusLED;
static LEDWidget sPumpStateLED;
static LEDWidget sUnusedLED;
static LEDWidget sUnusedLED_1;

static bool sIsThreadProvisioned = false;
static bool sIsThreadEnabled     = false;
static bool sHaveBLEConnections  = false;

static k_timer sFunctionTimer;

using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app::Clusters;

AppTask AppTask::sAppTask;

int AppTask::Init()
{
    // Initialize LEDs
    LEDWidget::InitGpio();
    LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);

    sStatusLED.Init(SYSTEM_STATE_LED);
    sPumpStateLED.Init(PUMP_STATE_LED);
    sPumpStateLED.Set(!PumpMgr().IsStopped());

    sUnusedLED.Init(DK_LED3);
    sUnusedLED_1.Init(DK_LED4);

    UpdateStatusLED();

    // Initialize buttons
    int ret = dk_buttons_init(ButtonEventHandler);
    if (ret)
    {
        LOG_ERR("dk_buttons_init() failed");
        return ret;
    }

    // Initialize timer user data
    k_timer_init(&sFunctionTimer, &AppTask::TimerEventHandler, nullptr);
    k_timer_user_data_set(&sFunctionTimer, this);

#ifdef CONFIG_MCUMGR_SMP_BT
    GetDFUOverSMP().Init(RequestSMPAdvertisingStart);
    GetDFUOverSMP().ConfirmNewImage();
#endif

    PumpMgr().Init();
    PumpMgr().SetCallbacks(ActionInitiated, ActionCompleted);

    // Init ZCL Data Model and start server
    chip::Server::GetInstance().Init();

    // Initialize device attestation config
    SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
    ConfigurationMgr().LogDeviceConfig();
    PrintOnboardingCodes(chip::RendezvousInformationFlag(chip::RendezvousInformationFlag::kBLE));

#if defined(CONFIG_CHIP_NFC_COMMISSIONING) || defined(CONFIG_MCUMGR_SMP_BT)
    PlatformMgr().AddEventHandler(ChipEventHandler, 0);
#endif
    return 0;
}

int AppTask::StartApp()
{
    int ret = Init();

    if (ret)
    {
        LOG_ERR("AppTask.Init() failed");
        return ret;
    }

    AppEvent event = {};

    while (true)
    {
        k_msgq_get(&sAppEventQueue, &event, K_FOREVER);
        DispatchEvent(&event);
    }
}

void AppTask::StartActionEventHandler(AppEvent * aEvent)
{
    PumpManager::Action_t action = PumpManager::INVALID_ACTION;
    int32_t actor                = 0;

    if (aEvent->Type == AppEvent::kEventType_Start)
    {
        action = static_cast<PumpManager::Action_t>(aEvent->StartEvent.Action);
        actor  = aEvent->StartEvent.Actor;
    }
    else if (aEvent->Type == AppEvent::kEventType_Button)
    {
        action = PumpMgr().IsStopped() ? PumpManager::START_ACTION : PumpManager::STOP_ACTION;
        actor  = AppEvent::kEventType_Button;
    }

    if (action != PumpManager::INVALID_ACTION && !PumpMgr().InitiateAction(actor, action))
        LOG_INF("Action is already in progress or active.");
}

void AppTask::ButtonEventHandler(uint32_t button_state, uint32_t has_changed)
{
    AppEvent button_event;
    button_event.Type = AppEvent::kEventType_Button;

    if (START_BUTTON_MASK & button_state & has_changed)
    {
        button_event.ButtonEvent.PinNo  = START_BUTTON;
        button_event.ButtonEvent.Action = BUTTON_PUSH_EVENT;
        button_event.Handler            = StartActionEventHandler;
        sAppTask.PostEvent(&button_event);
    }

    if (FUNCTION_BUTTON_MASK & has_changed)
    {
        button_event.ButtonEvent.PinNo  = FUNCTION_BUTTON;
        button_event.ButtonEvent.Action = (FUNCTION_BUTTON_MASK & button_state) ? BUTTON_PUSH_EVENT : BUTTON_RELEASE_EVENT;
        button_event.Handler            = FunctionHandler;
        sAppTask.PostEvent(&button_event);
    }

    if (THREAD_START_BUTTON_MASK & button_state & has_changed)
    {
        button_event.ButtonEvent.PinNo  = THREAD_START_BUTTON;
        button_event.ButtonEvent.Action = BUTTON_PUSH_EVENT;
        button_event.Handler            = StartThreadHandler;
        sAppTask.PostEvent(&button_event);
    }

    if (BLE_ADVERTISEMENT_START_BUTTON_MASK & button_state & has_changed)
    {
        button_event.ButtonEvent.PinNo  = BLE_ADVERTISEMENT_START_BUTTON;
        button_event.ButtonEvent.Action = BUTTON_PUSH_EVENT;
        button_event.Handler            = StartBLEAdvertisementHandler;
        sAppTask.PostEvent(&button_event);
    }
}

void AppTask::TimerEventHandler(k_timer * timer)
{
    AppEvent event;
    event.Type               = AppEvent::kEventType_Timer;
    event.TimerEvent.Context = k_timer_user_data_get(timer);
    event.Handler            = FunctionTimerEventHandler;
    sAppTask.PostEvent(&event);
}

void AppTask::FunctionTimerEventHandler(AppEvent * aEvent)
{
    if (aEvent->Type != AppEvent::kEventType_Timer)
        return;

    // If we reached here, the button was held past FACTORY_RESET_TRIGGER_TIMEOUT, initiate factory reset
    if (sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_SoftwareUpdate)
    {
        LOG_INF("Factory Reset Triggered. Release button within %ums to cancel.", FACTORY_RESET_TRIGGER_TIMEOUT);

        // Start timer for FACTORY_RESET_CANCEL_WINDOW_TIMEOUT to allow user to cancel, if required.
        sAppTask.StartTimer(FACTORY_RESET_CANCEL_WINDOW_TIMEOUT);
        sAppTask.mFunction = kFunction_FactoryReset;

        // Turn off all LEDs before starting blink to make sure blink is co-ordinated.
        sStatusLED.Set(false);
        sPumpStateLED.Set(false);
        sUnusedLED_1.Set(false);
        sUnusedLED.Set(false);

        sStatusLED.Blink(500);
        sPumpStateLED.Blink(500);
        sUnusedLED.Blink(500);
        sUnusedLED_1.Blink(500);
    }
    else if (sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_FactoryReset)
    {
        // Actually trigger Factory Reset
        sAppTask.mFunction = kFunction_NoneSelected;
        ConfigurationMgr().InitiateFactoryReset();
    }
}

#ifdef CONFIG_MCUMGR_SMP_BT
void AppTask::RequestSMPAdvertisingStart(void)
{
    AppEvent event;
    event.Type    = AppEvent::kEventType_StartSMPAdvertising;
    event.Handler = [](AppEvent *) { GetDFUOverSMP().StartBLEAdvertising(); };
    sAppTask.PostEvent(&event);
}
#endif

void AppTask::FunctionHandler(AppEvent * aEvent)
{
    if (aEvent->ButtonEvent.PinNo != FUNCTION_BUTTON)
        return;

    // To trigger software update: press the FUNCTION_BUTTON button briefly (< FACTORY_RESET_TRIGGER_TIMEOUT)
    // To initiate factory reset: press the FUNCTION_BUTTON for FACTORY_RESET_TRIGGER_TIMEOUT + FACTORY_RESET_CANCEL_WINDOW_TIMEOUT
    // All LEDs start blinking after FACTORY_RESET_TRIGGER_TIMEOUT to signal factory reset has been initiated.
    // To cancel factory reset: release the FUNCTION_BUTTON once all LEDs start blinking within the
    // FACTORY_RESET_CANCEL_WINDOW_TIMEOUT
    if (aEvent->ButtonEvent.Action == BUTTON_PUSH_EVENT)
    {
        if (!sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_NoneSelected)
        {
            sAppTask.StartTimer(FACTORY_RESET_TRIGGER_TIMEOUT);

            sAppTask.mFunction = kFunction_SoftwareUpdate;
        }
    }
    else
    {
        // If the button was released before factory reset got initiated, trigger a software update.
        if (sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_SoftwareUpdate)
        {
            sAppTask.CancelTimer();
            sAppTask.mFunction = kFunction_NoneSelected;

#ifdef CONFIG_MCUMGR_SMP_BT
            GetDFUOverSMP().StartServer();
#else
            LOG_INF("Software update is disabled");
#endif
        }
        else if (sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_FactoryReset)
        {
            sUnusedLED.Set(false);
            sUnusedLED_1.Set(false);

            // Set pump state LED back to show state of pump.
            sPumpStateLED.Set(!PumpMgr().IsStopped());

            UpdateStatusLED();
            sAppTask.CancelTimer();

            // Change the function to none selected since factory reset has been canceled.
            sAppTask.mFunction = kFunction_NoneSelected;

            LOG_INF("Factory Reset has been Canceled");
        }
    }
}

void AppTask::StartThreadHandler(AppEvent * aEvent)
{
    if (aEvent->ButtonEvent.PinNo != THREAD_START_BUTTON)
        return;

    if (chip::Server::GetInstance().AddTestCommissioning() != CHIP_NO_ERROR)
    {
        LOG_ERR("Failed to add test pairing");
    }

    if (!ConnectivityMgr().IsThreadProvisioned())
    {
        StartDefaultThreadNetwork();
        LOG_INF("Device is not commissioned to a Thread network. Starting with the default configuration.");
    }
    else
    {
        LOG_INF("Device is commissioned to a Thread network.");
    }
}

void AppTask::StartBLEAdvertisementHandler(AppEvent * aEvent)
{
    if (aEvent->ButtonEvent.PinNo != BLE_ADVERTISEMENT_START_BUTTON)
        return;

    // Don't allow on starting Matter service BLE advertising after Thread provisioning.
    if (ConnectivityMgr().IsThreadProvisioned())
    {
        LOG_INF("NFC Tag emulation and Matter service BLE advertising not started - device is commissioned to a Thread network.");
        return;
    }

    if (ConnectivityMgr().IsBLEAdvertisingEnabled())
    {
        LOG_INF("BLE advertising is already enabled");
        return;
    }

    if (chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow() != CHIP_NO_ERROR)
    {
        LOG_ERR("OpenBasicCommissioningWindow() failed");
    }
}

void AppTask::UpdateLedStateEventHandler(AppEvent * aEvent)
{
    if (aEvent->Type == AppEvent::kEventType_UpdateLedState)
    {
        aEvent->UpdateLedStateEvent.LedWidget->UpdateState();
    }
}

void AppTask::LEDStateUpdateHandler(LEDWidget & ledWidget)
{
    AppEvent event;
    event.Type                          = AppEvent::kEventType_UpdateLedState;
    event.Handler                       = UpdateLedStateEventHandler;
    event.UpdateLedStateEvent.LedWidget = &ledWidget;
    sAppTask.PostEvent(&event);
}

void AppTask::UpdateStatusLED()
{
    /* Update the status LED.
     *
     * If thread and service provisioned, keep the LED On constantly.
     *
     * If the system has ble connection(s) uptill the stage above, THEN blink the LED at an even
     * rate of 100ms.
     *
     * Otherwise, blink the LED On for a very short time. */
    if (sIsThreadProvisioned && sIsThreadEnabled)
    {
        sStatusLED.Set(true);
    }
    else if (sHaveBLEConnections)
    {
        sStatusLED.Blink(100, 100);
    }
    else
    {
        sStatusLED.Blink(50, 950);
    }
}

void AppTask::ChipEventHandler(const ChipDeviceEvent * event, intptr_t /* arg */)
{
    switch (event->Type)
    {
    case DeviceEventType::kCHIPoBLEAdvertisingChange:
        if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Stopped)
        {
#ifdef CONFIG_CHIP_NFC_COMMISSIONING
            NFCMgr().StopTagEmulation();
#endif
#ifdef CONFIG_MCUMGR_SMP_BT
            // After CHIPoBLE advertising stop, start advertising SMP in case Thread is enabled or there are no active CHIPoBLE
            // connections (exclude the case when CHIPoBLE advertising is stopped on the connection time)
            if (GetDFUOverSMP().IsEnabled() &&
                (ConnectivityMgr().IsThreadProvisioned() || ConnectivityMgr().NumBLEConnections() == 0))
                sAppTask.RequestSMPAdvertisingStart();
#endif
        }
#ifdef CONFIG_CHIP_NFC_COMMISSIONING
        else if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Started)
        {
            if (NFCMgr().IsTagEmulationStarted())
            {
                LOG_INF("NFC Tag emulation is already started");
            }
            else
            {
                ShareQRCodeOverNFC(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
            }
        }
#endif
        sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
        UpdateStatusLED();
        break;
    case DeviceEventType::kThreadStateChange:
        sIsThreadProvisioned = ConnectivityMgr().IsThreadProvisioned();
        sIsThreadEnabled     = ConnectivityMgr().IsThreadEnabled();
        UpdateStatusLED();
        break;
    default:
        break;
    }
}

void AppTask::CancelTimer()
{
    k_timer_stop(&sFunctionTimer);
    mFunctionTimerActive = false;
}

void AppTask::StartTimer(uint32_t aTimeoutInMs)
{
    k_timer_start(&sFunctionTimer, K_MSEC(aTimeoutInMs), K_NO_WAIT);
    mFunctionTimerActive = true;
}

void AppTask::ActionInitiated(PumpManager::Action_t aAction, int32_t aActor)
{
    // If the action has been initiated by the pump, update the pump trait
    // and start flashing the LEDs rapidly to indicate action initiation.
    if (aAction == PumpManager::START_ACTION)
    {
        LOG_INF("Pump Start Action has been initiated");
    }
    else if (aAction == PumpManager::STOP_ACTION)
    {
        LOG_INF("Pump Stop Action has been initiated");
    }

    sPumpStateLED.Blink(50, 50);
}

void AppTask::ActionCompleted(PumpManager::Action_t aAction, int32_t aActor)
{
    // If the action has been completed by the pump, update the pump trait.
    // Turn on the pump state LED if in a STARTED state OR
    // Turn off the pump state LED if in a STOPPED state.
    if (aAction == PumpManager::START_ACTION)
    {
        LOG_INF("Pump Start Action has been completed");
        sPumpStateLED.Set(true);
    }
    else if (aAction == PumpManager::STOP_ACTION)
    {
        LOG_INF("Pump Stop Action has been completed");
        sPumpStateLED.Set(false);
    }

    if (aActor == AppEvent::kEventType_Button)
    {
        sAppTask.UpdateClusterState();
    }
}

void AppTask::PostStartActionRequest(int32_t aActor, PumpManager::Action_t aAction)
{
    AppEvent event;
    event.Type              = AppEvent::kEventType_Start;
    event.StartEvent.Actor  = aActor;
    event.StartEvent.Action = aAction;
    event.Handler           = StartActionEventHandler;
    PostEvent(&event);
}

void AppTask::PostEvent(AppEvent * aEvent)
{
    if (k_msgq_put(&sAppEventQueue, aEvent, K_NO_WAIT))
    {
        LOG_INF("Failed to post event to app task event queue");
    }
}

void AppTask::DispatchEvent(AppEvent * aEvent)
{
    if (aEvent->Handler)
    {
        aEvent->Handler(aEvent);
    }
    else
    {
        LOG_INF("Event received with no handler. Dropping event.");
    }
}

void AppTask::UpdateClusterState()
{
    EmberStatus status;

    ChipLogProgress(NotSpecified, "UpdateClusterState");

    // Write the new values

    bool onOffState = !PumpMgr().IsStopped();

    status = OnOff::Attributes::OnOff::Set(ONOFF_CLUSTER_ENDPOINT, onOffState);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating On/Off state  %x", status);
    }

    int16_t maxPressure = PumpMgr().GetMaxPressure();
    status              = PumpConfigurationAndControl::Attributes::MaxPressure::Set(PCC_CLUSTER_ENDPOINT, maxPressure);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MaxPressure  %x", status);
    }

    uint16_t maxSpeed = PumpMgr().GetMaxSpeed();
    status            = PumpConfigurationAndControl::Attributes::MaxSpeed::Set(PCC_CLUSTER_ENDPOINT, maxSpeed);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MaxSpeed  %x", status);
    }

    uint16_t maxFlow = PumpMgr().GetMaxFlow();
    status           = PumpConfigurationAndControl::Attributes::MaxFlow::Set(PCC_CLUSTER_ENDPOINT, maxFlow);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MaxFlow  %x", status);
    }

    int16_t minConstPress = PumpMgr().GetMinConstPressure();
    status                = PumpConfigurationAndControl::Attributes::MinConstPressure::Set(PCC_CLUSTER_ENDPOINT, minConstPress);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MinConstPressure  %x", status);
    }

    int16_t maxConstPress = PumpMgr().GetMaxConstPressure();
    status                = PumpConfigurationAndControl::Attributes::MaxConstPressure::Set(PCC_CLUSTER_ENDPOINT, maxConstPress);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MaxConstPressure  %x", status);
    }

    int16_t minCompPress = PumpMgr().GetMinCompPressure();
    status               = PumpConfigurationAndControl::Attributes::MinCompPressure::Set(PCC_CLUSTER_ENDPOINT, minCompPress);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MinCompPressure  %x", status);
    }

    int16_t maxCompPress = PumpMgr().GetMaxCompPressure();
    status               = PumpConfigurationAndControl::Attributes::MaxCompPressure::Set(PCC_CLUSTER_ENDPOINT, maxCompPress);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MaxCompPressure  %x", status);
    }

    uint16_t minConstSpeed = PumpMgr().GetMinConstSpeed();
    status                 = PumpConfigurationAndControl::Attributes::MinConstSpeed::Set(PCC_CLUSTER_ENDPOINT, minConstSpeed);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MinConstSpeed  %x", status);
    }

    uint16_t maxConstSpeed = PumpMgr().GetMaxConstSpeed();
    status                 = PumpConfigurationAndControl::Attributes::MaxConstSpeed::Set(PCC_CLUSTER_ENDPOINT, maxConstSpeed);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MaxConstSpeed  %x", status);
    }

    uint16_t minConstFlow = PumpMgr().GetMinConstFlow();
    status                = PumpConfigurationAndControl::Attributes::MinConstFlow::Set(PCC_CLUSTER_ENDPOINT, minConstFlow);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MinConstFlow  %x", status);
    }

    uint16_t maxConstFlow = PumpMgr().GetMaxConstFlow();
    status                = PumpConfigurationAndControl::Attributes::MaxConstFlow::Set(PCC_CLUSTER_ENDPOINT, maxConstFlow);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MaxConstFlow  %x", status);
    }

    int16_t minConstTemp = PumpMgr().GetMinConstTemp();
    status               = PumpConfigurationAndControl::Attributes::MinConstTemp::Set(PCC_CLUSTER_ENDPOINT, minConstTemp);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MinConstTemp  %x", status);
    }

    int16_t maxConstTemp = PumpMgr().GetMaxConstTemp();
    status               = PumpConfigurationAndControl::Attributes::MaxConstTemp::Set(PCC_CLUSTER_ENDPOINT, maxConstTemp);
    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        ChipLogError(NotSpecified, "ERR: Updating MaxConstTemp  %x", status);
    }
}
