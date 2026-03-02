// TODO(Tuuli): Mutexes, semaphors, thread protection? Any shared resources?

// TODO(Tuuli): write tests

// TODO(XXX): Audio playback / bpm is noticeably slower when mixxx isnt the focussed window; no difference with/without MCO enabled

// TODO(Tuuli): midi out methods, 0xF8, and selecting a Midi device in a controller-mapping, controller thread. 
// Would it make sense to use MidiThroughPort, or open a similar virtual port, for MidiClockOut signals?
// Testing qObject->parent() recursion failed; looks like EngineSync doesnt have a parent assigned.
// Using CoreServices to Connect events across threads, and detecting controllers that have Midi Clock Out assigned. Some bugs.
// Make Controller::sendBytes() invokable, or add a slotSendBytes? Should MidiClockOut use the Controller resource directly, or via signal/slots that allow messages to be sent in the Controller thread? 

// TODO(Tuuli): Should portMidi device have a buffer and timestamps? It might help, for beatjumping ahead especially.. Saw this too... " warning [Controller] PortMidi error: PortMidi: Buffer overflow" is that the output or input buffer? Does the event queue act as a buffer of-sorts for portmidicontroller output?
// TODO(Tuuli) Sequence is vital for MIDI messages; make sure they are never sent out of order. How is this handled? Start-Stop is very different outcome from Stop-Start. Events are always delivered in-order by Qt.
// TODO(XXX) Fix the PortMidiController::sendBytes or Controller MIDI message parser? Is Arduinos MIDI.h open-source compatible with Mixxx, can we use that? Controller::sendBytes doesnt work for non-sysex Midi messages and assumes all messages are sysex... but its the only inherited MIDI sending function. Currently hacked to allow F8, FA, FB, FC

// TODO(Tuuli): Send Continue FB on Enable without a stop; check if Stop-Start is best approach for restart, or if just Start will be sufficient as a default. Add settings option to enable Continue messages for users to choose what their sequencers receive
 
// TODO(Tuuli): MidiClockOut should be controllable if its the only item playing.     
    // The clock should keep playing at the previous tempo; 
    // BUT... how to change tempo now? To drive external synths?
    
    // Could have a deck's tempo-fader mapped, and grab the last clock leaders
    // fader to now control the MIDI Clock.
     
    // We could have a pre-programmed switch-over to internal clock (SYSEX)
    // and midi.setTempo() to the current tempo as a fall-back, so that 
    // external synths can use their own clocks if nothing is playing on Mixxx.
    // Not all clock followers will support a SYSEX command to switch their clocks.
    // But its probably the best we can do...

// TODO(Tuuli): BUG Not grabbing BPM when another playing syncable becomes leader - fixed now? Test, and remove redundant BPM-hoarding..
    //If not, find a way to get Bpm...
    // auto otherBpm = m_pEngineSync->leaderBpm(); // private function
    // auto otherBeatDistance = m_pEngineSync->leaderBaseBpm(); //private function

    // This is the bad way to do it... DONT TRY THIS; a hack, and not thread-safe...
    // EngineChannel* pLeaderChannel = m_pEngineSync->getLeaderChannel();
    // auto leaderBpm = pLeaderChannel->getEngineBuffer()->getBpm();

    // other experiments
    //pLeaderChannel->EngineBuffer::getBpm()--
    //  m_pBpmControl->BpmControl::getBpm() --
    //      m_pEngineBpm->get() --
    //          std::unique_ptr<ControlLinPotmeter> m_pEngineBpm --
    //              ControlObject->get()->m_value.get()-- gets value atomically

    //SyncControl::setEngineControls(BpmControl::pBpmControl)

    // currently the clock doesnt adopt tempo of a new leader until that leader changes their BPM...
    // Need to capture a notice about the new leader and then pull their BPM.

// TODO(Tuuli) BUG/incomplete following tempo changes doesnt work fully yet, it drifts off-beat from tempo changes. Lock to the beat position

// Direct access to Controller:
//TODO(Tuuli) BUG Crashing when swapping mapped devices (probably garbage collection / pointer issues) Need to handle updates from ControllerManager similarly to initializing and shutdowns (probably can use the same functions / signals already wired up)
//TODO(Tuuli) Try again to put threads back in
//TODO(Tuuli) BUG Why no messages received by MIDI-OX, was it just that portmidi errored out after a buffer overflow or something? Try to reproduce by crashing / overflowing portmidi and see what happens... Swapping devices seemed to fix it last time...

#include "engine/sync/midiclockout.h"

#include <QtDebug>
#include <cmath>

//TODO(Tuuli): These libraries are included in the .h already, remove?
#include <chrono>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QChronoTimer>
#else
#include <QTimer>
#endif

#include "control/controlobject.h"
#include "control/controlindicatortimer.h"
#include "engine/sync/enginesync.h"
#include "moc_midiclockout.cpp"
#include "preferences/usersettings.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("MidiClockOut");
constexpr mixxx::Bpm kDefaultBpm(9999.9);
constexpr mixxx::Bpm kStartBpm(120.0);
constexpr std::chrono::microseconds kStartTickLength{(int)(2500000.0 / 120.0)};
constexpr std::chrono::microseconds ktickCutOff{300};
} // namespace

MidiClockOut::MidiClockOut(const QString& group, EngineSync* pEngineSync)
        : m_group(group),
          m_pEngineSync(pEngineSync),
          m_syncMode(SyncMode::None),
          m_currentBpm(kStartBpm),          
          m_absTimeWhenPrevOutputBufferReachesDac(0),
          mV_tempoChangeSyncAdjustment(0),
          mV_currentTickLength(kStartTickLength),
          m_newTickLength(kStartTickLength),          

          m_enabled(false),
          m_tickCount(0),
          m_sixteenths(1),
          m_beats(1),
          m_bars(1),

          mflag_bpmChangedThisBar(false),          
          mV_ticksSinceBpmChange(0),          
          mV_tickAdjustment(0),

          m_ticknsTimerID(Qt::TimerId::Invalid),
          m_pMidiClockOutThread(std::make_unique<MidiClockOutThread>(this)),
          m_debugTickCounter(0),
          m_timingStyleThread(false),
          m_midiStyleThread(true),
          // GUI objects
          m_pMidiClockEnableButton(std::make_unique<ControlPushButton>(ConfigKey(group, "out_enabled"))),
          m_pMidiClockRestartButton(std::make_unique<ControlPushButton>(ConfigKey(group, "restart"))),
          m_pMidiClockTickButton(std::make_unique<ControlPushButton>(ConfigKey(group, "tick"))),
          m_pMidiClockNudgeFwdButton(std::make_unique<ControlPushButton>(ConfigKey(group, "nudge_fwd"))),
          m_pMidiClockNudgeBackButton(std::make_unique<ControlPushButton>(ConfigKey(group, "nudge_back"))),
          m_pMidiClockPosSixteenths(std::make_unique<ControlObject>(ConfigKey(group, "num_sixteenths"))),
          m_pMidiClockPosBeats(std::make_unique<ControlObject>(ConfigKey(group, "num_beats"))),
          m_pMidiClockPosBars(std::make_unique<ControlObject>(ConfigKey(group, "num_bars"))),
          // Midi control objects to link to the JS script
          m_pMidiClockTick(std::make_unique<ControlObject>(ConfigKey(group, "clock_tick"), false)),
          m_pMidiClockStart(std::make_unique<ControlObject>(ConfigKey(group, "clock_start"), false)),
          m_pMidiClockContinue(std::make_unique<ControlObject>(ConfigKey(group, "clock_continue"), false)),
          m_pMidiClockStop(std::make_unique<ControlObject>(ConfigKey(group, "clock_stop"), false)) {
    // Setup GUI
    // ControlIndicatorTimer
    m_pMidiClockEnableButton->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pMidiClockEnableButton->setStates(2);
    QObject::connect(m_pMidiClockEnableButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlOutEnabled);

    m_pMidiClockRestartButton->setButtonMode(mixxx::control::ButtonMode::Trigger);
    m_pMidiClockRestartButton->setStates(1);
    QObject::connect(m_pMidiClockRestartButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlRestart);

    m_pMidiClockTickButton->setButtonMode(mixxx::control::ButtonMode::Trigger);
    m_pMidiClockTickButton->setStates(1);
    QObject::connect(m_pMidiClockTickButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlTick);

    m_pMidiClockNudgeFwdButton->setButtonMode(mixxx::control::ButtonMode::Trigger);
    m_pMidiClockNudgeFwdButton->setStates(1);
    QObject::connect(m_pMidiClockNudgeFwdButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlNudgeFwd);

    m_pMidiClockNudgeBackButton->setButtonMode(mixxx::control::ButtonMode::Trigger);
    m_pMidiClockNudgeBackButton->setStates(1);
    QObject::connect(m_pMidiClockNudgeBackButton.get(),
            &ControlObject::valueChanged,
            this,
            &MidiClockOut::slotControlNudgeBack);

    m_pMidiClockPosSixteenths->setReadOnly();
    m_pMidiClockPosSixteenths->forceSet(m_sixteenths);

    m_pMidiClockPosBeats->setReadOnly();
    m_pMidiClockPosBeats->forceSet(m_beats);

    m_pMidiClockPosBars->setReadOnly();
    m_pMidiClockPosBars->forceSet(m_bars);

    //Other setup

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        qDebug() << "MidiClockOut::constructor(): Qt>=6.8, using QChronoTimer; nanosecond resolution";
#else
        qDebug() << "MidiClockOut::constructor(): Qt<6.8, using QTimer; millisecond resolution only";
#endif
    m_ticknsTimer.setParent(this);
    m_ticknsTimer.callOnTimeout(this, &MidiClockOut::slotDummy);
    m_ticknsTimer.setSingleShot(true);
           
    m_timeReceivedNewBpm = mV_adjustedTimeReceivedNewBpm = std::chrono::steady_clock::now(); // this is temporary; ideally overwritten when m_enabled is updated       
        
    if (m_timingStyleThread || m_midiStyleThread) {
        m_pMidiClockOutThread->startMidiClockOutThread();
        QObject::connect(m_pMidiClockOutThread.get(), &MidiClockOutThread::tickSent, this, &MidiClockOut::tick);
    }
    // audioThreadDebugOutput();
    qDebug() << "MidiClockOut::constructor() done";
}

MidiClockOut::~MidiClockOut() {
    // TODO(Tuuli): Setup a SYSEX command to optionally be sent on exit, that would tell
    // external followers to switch their clocks to internal mode, and set their BPMs
    // with midi.setTempo()
    // (not all sequencers work like LP Pro firmware and treat external 0xF8 as tap tempo..)

    m_enabled = false;

    // Disconnect the connection from the GUI buttons
    if (m_pMidiClockEnableButton) {
        QObject::disconnect(m_pMidiClockEnableButton.get(),
                &ControlObject::valueChanged,
                this,
                &MidiClockOut::slotControlOutEnabled);
    }

    if (m_pMidiClockRestartButton) {
        QObject::disconnect(m_pMidiClockRestartButton.get(),
                &ControlObject::valueChanged,
                this,
                &MidiClockOut::slotControlRestart);
    }

    if (m_pMidiClockTickButton) {
        QObject::disconnect(m_pMidiClockTickButton.get(),
                &ControlObject::valueChanged,
                this,
                &MidiClockOut::slotControlTick);
    }

    if (m_pMidiClockNudgeFwdButton) {
        QObject::disconnect(m_pMidiClockNudgeFwdButton.get(),
                &ControlObject::valueChanged,
                this,
                &MidiClockOut::slotControlNudgeFwd);
    }

    if (m_pMidiClockNudgeBackButton) {
        QObject::disconnect(m_pMidiClockNudgeBackButton.get(),
                &ControlObject::valueChanged,
                this,
                &MidiClockOut::slotControlNudgeBack);
    }

    // Destroy control objects before releasing MidiClockOut
    m_pMidiClockOutThread->stopPlease();
    qDebug() << "MidiClockOut::destructor: Asked MidiClockOutThread to stop";
    qDebug() << "MidiClockOut::destructor: MidiClockOutThread resetting..";
    //m_pMidiClockOutThread.reset(); // TODO(Tuuli): sometimes EngineMixer fails to exit, is this the bug?
    qDebug() << "MidiClockOut::destroyed";

    m_pMidiClockEnableButton.reset();
    m_pMidiClockRestartButton.reset();
    m_pMidiClockTickButton.reset();
    m_pMidiClockNudgeFwdButton.reset();
    m_pMidiClockNudgeBackButton.reset();
   
    m_pMidiClockPosSixteenths.reset();
    m_pMidiClockPosBeats.reset();
    m_pMidiClockPosBars.reset();

    m_pMidiClockTick.reset();
    m_pMidiClockStart.reset();
    m_pMidiClockContinue.reset();
    m_pMidiClockStop.reset();

    //Destroy pointer safely
    deleteMidiClockOutController();
}
void MidiClockOut::slotDummy() {
    qDebug() << "MidiClockOut::slotDummy";
}

void MidiClockOut::setMidiClockOutController(Controller* pMidiClockOutController) {
    m_pMidiClockOutController = pMidiClockOutController;
    m_pMidiClockOutThread->setMidiClockOutController(pMidiClockOutController);
}
void MidiClockOut::deleteMidiClockOutController() {
    m_pMidiClockOutController = nullptr;
    m_pMidiClockOutThread->deleteMidiClockOutController();
}

// GUI Controls
// Slots are called from the [Main] thread, so anything it calls is also called from [Main]

void MidiClockOut::slotControlOutEnabled(double controlButtonValue) {
    DEBUG_ASSERT(m_currentBpm.isReasonable());
    
    qDebug() << "MidiClockOut::slotControlOutEnabled():" << controlButtonValue;
    m_enabled = (controlButtonValue > 0);
    m_pMidiClockOutThread->setBeatClockState(m_enabled);

    if (m_enabled) {  
        sendMidiClockStart();

        m_timeReceivedNewBpm = mV_adjustedTimeReceivedNewBpm = std::chrono::steady_clock::now();   
        mV_ticksSinceBpmChange = 0;
        mV_tempoChangeSyncAdjustment = std::chrono::nanoseconds(0);
        mV_currentTickLength = tickLengthFromBpm(m_currentBpm.value());                 

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
                mV_currentTickLength));
#else
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(
                mV_currentTickLength));
#endif
        m_ticknsTimer.start();            
        m_ticknsTimerID = m_ticknsTimer.id();                                
        m_pEngineSync->requestSyncMode(this, SyncMode::Follower);
        m_pEngineSync->notifyPlayingAudible(this, true); // TODO(Tuuli): is this required? Will we get the leaders tempo if we're audible?        
        //updateLeaderBpm(m_currentBpm); // Setup a new BPM change        
    } 
    else {
        m_ticknsTimer.stop();
        sendMidiClockStop();        
        m_pEngineSync->requestSyncMode(this, SyncMode::None);     
        m_pEngineSync->notifyPlayingAudible(this, false); // TODO(Tuuli): is this required?
    }
}

void MidiClockOut::slotControlRestart(double controlButtonValue) {    
    Q_UNUSED(controlButtonValue)
    qDebug() << "MidiClockOut::slotControlRestart()";
    forceGetBeatDistance(); /// Ask for the beatDistance from the leader at the moment of user restarting MidiClockOut; does nothing
    restart();    
}

void MidiClockOut::slotControlTick(double controlButtonValue) {
    Q_UNUSED(controlButtonValue)
    qDebug() << "MidiClockOut::slotControlTick";            
    tick();
}

void MidiClockOut::slotControlNudgeFwd(double controlButtonValue) {
    Q_UNUSED(controlButtonValue)
    fwdSixteenth();
}

void MidiClockOut::slotControlNudgeBack(double controlButtonValue) {
    Q_UNUSED(controlButtonValue)
    backSixteenth();    
}

/// Syncable overrides

/// Notify a Syncable that their mode has changed. The Syncable must record
/// this mode and return the latest mode in response to getMode().
void MidiClockOut::setSyncMode(SyncMode syncMode) {
    m_syncMode = syncMode;
    qDebug() << "MidiClockOut::setSyncMode(), syncMode:" << syncMode;
}
/// Notify a Syncable that it is now the only currently-playing syncable.
void MidiClockOut::notifyUniquePlaying() {
    qDebug() << "MidiClockOut::notifyUniquePlaying()";
}
/// Notify a Syncable that they should sync phase.
void MidiClockOut::requestSync() {     
    // updateLeaderBeatDistance(double beatDistance) handles the sync position update.
    // 
    // SyncControl::requestSync() calls m_pChannel->getEngineBuffer()->requestSyncPhase(); only.
    // This queues a phase seek, then handled in EngineBuffer::processSeek() function
    // It reads the current m_playPos and shapes it through these checks...
    // syncPosition = m_pBpmControl->getBeatMatchPosition(m_playPos, true, true);
    // position = m_pLoopingControl->getSyncPositionInsideLoop(m_playPos, syncPosition);  
    // It then calls EngineBuffer::setNewPlaypos(position) to set EngineBuffer::m_playPos if they arent equal
    // 
    // EngineSync::requestSyncMode() calls:
    //   reinitLeaderParams(pParamsSyncable);
    //   pSyncable->updateInstantaneousBpm(pParamsSyncable->getBpm());
    //   if (pParamsSyncable != pSyncable && mode != SyncMode::None) pSyncable->requestSync();    
}
/// Must NEVER return a mode that was not set directly via
/// notifySyncModeChanged.
SyncMode MidiClockOut::getSyncMode() const {
    return m_syncMode;
}
bool MidiClockOut::isPlaying() const {
    return m_enabled;
}
bool MidiClockOut::isAudible() const {
    // TODO(Tuuli): Should this be marked audible? Potentially external drum machines are.
    return m_enabled;
}
bool MidiClockOut::isQuantized() const {
    // TODO(Tuuli): Should this be whether it SHOULD BE quantized, or if it thinks its already on-beat?
    return m_enabled;
}
mixxx::Bpm MidiClockOut::getBpm() const {
    return m_currentBpm;
}
double MidiClockOut::getBeatDistance() const {    
    return std::fmod(m_tickCount, 24.0);
}
mixxx::Bpm MidiClockOut::getBaseBpm() const {    
    return m_currentBpm; //TODO(Tuuli): whats this for? Half/double?
}

void MidiClockOut::updateLeaderBeatDistance(double beatDistance) {    
    qDebug() << "MidiClockOut::updateLeaderBeatDistance()";
    // TODO(Tuuli) add a setting or control for toggling following sync changes
    // TODO(Tuuli) We dont store the desired offset between the tick position, and the music's beatDistance; need to get the live Leader_beatDistance at the time of a GUI enable or restart command
    // TODO(Tuuli) This info is only stored for now
    m_beatDistance = beatDistance;
    m_timeReceivedBeatDistance = std::chrono::steady_clock::now();
    m_ticksReceivedBeatDistance = m_tickCount;

    auto tickSyncOffset = (int32_t)(beatDistance * 24) - ((int32_t)m_tickCount % 24); // TODO(Tuuli): do we want sequencers to keep playing in place, and catch up?
    resetQueuedSyncTicks();
    adjustSyncTicks(tickSyncOffset);

    auto threadSyncOffset = m_pMidiClockOutThread->setBeatPosAtTime(m_timeReceivedBeatDistance, beatDistance);
}

void MidiClockOut::forceUpdateLeaderBeatDistance(double beatDistance) {
    qDebug() << "MidiClockOut::forceUpdateLeaderBeatDistance()";
    updateLeaderBeatDistance(beatDistance);    
}

//SyncControl::slotRateChanged() (Syncable leader's synccontrol) calls m_pEngineSync->notifyRateChanged(this, bpm / m_leaderBpmAdjustFactor);
//EngineSync::notifyRateChanged calls EngineSync::updateLeaderBpm(pSyncable source, bpm);
// TODO(Tuuli): should MidiClockOut be one of EngineSync::m_syncables? Probably not; m_syncables is used to choose sync leaders
void MidiClockOut::updateLeaderBpm(mixxx::Bpm bpm) {
    qDebug() << "MidiClockOut::updateLeaderBpm()" << bpm.value();       
    handleNewBPM(bpm);            
}

// TODO(Tuuli): Do we need to anything here?
// This resets double/half tempo beat rates and resets the rate to the true rate when the Syncable becomes the leader
void MidiClockOut::notifyLeaderParamSource() {
}

void MidiClockOut::reinitLeaderParams(double beatDistance, mixxx::Bpm, mixxx::Bpm bpm) {
    updateLeaderBeatDistance(beatDistance);
    updateLeaderBpm(bpm);
}

void MidiClockOut::updateInstantaneousBpm(mixxx::Bpm bpm) {
    qDebug() << "MidiClockOut::updateInstantaneousBpm()";
    // updateLeaderBpm(bpm); /// Instantaneous is for scratching-only. Beatgrids instead use localBPM, which calls updateLeaderBpm
}

// Class functions
 
void MidiClockOut::forceGetBeatDistance() {
    // TODO(Tuuli): Can we safely ask for the current beatDistance? Does requestSync() trigger updateLeaderBeatDistance() calls if the requestSync() results in a beatDistance change?
    // TODO(Tuuli): Is this the correct way to get the leaders phase? Syncable::getBaseBpm() and m_pEngineSync->pickNonSyncSyncTarget() ...getEngineBuffer has a comment to remove it as its a hack. Replace with?
    //EngineChannel* pLeaderChannel = m_pEngineSync->getLeaderChannel();
    //newBeatDistance = pLeaderChannel->getEngineBuffer()->getExactPlayPos();

    // BpmControl::calcSyncAdjustment handles sync.
    // BpmControl::m_dSyncTargetBeatDistance stores the beatDistance
    // BpmControl::setTargetBeatDistance sets the value; 
    // called from SyncControl::updateTargetBeatDistance
    // called from EngineBuffer::postProcess; and called from SyncControl::updateLeaderBeatDistance
}

std::chrono::microseconds MidiClockOut::tickLengthFromBpm(double bpm) {
    //[us/t] = 1000000 [us/s] * 60 [s/min] / (24 [t/b] * bpm [b/min]) =
    // 2500000 / bpm   
    DEBUG_ASSERT(bpm >= 0);
    qDebug() << "MidiClockOut::tickLengthFromBpm(), bpm:" << bpm;
    std::chrono::microseconds conversion{(int)(2'500'000.0 / bpm)};
    return conversion;
}

void MidiClockOut::restart() {
    qDebug() << "MidiClockOut::restart(), enabled: " << m_enabled;

    mV_adjustedTimeReceivedNewBpm = std::chrono::steady_clock::now();
    auto beatResetPos = m_pMidiClockOutThread->resetBeatTimerPos(mV_adjustedTimeReceivedNewBpm);

    sendMidiClockStop();
    if (m_enabled) {        
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
                mV_currentTickLength));
#else
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(
                mV_currentTickLength));
#endif
        m_ticknsTimer.start();
        sendMidiClockStart();
        m_ticknsTimerID = m_ticknsTimer.id();        
    } 
    
    mV_tempoChangeSyncAdjustment = std::chrono::nanoseconds(0);    
    m_tickCount = 0;
    mV_ticksSinceBpmChange = 0;      
    resetQueuedSyncTicks();
    
    m_sixteenths = 1;
    m_beats = 1;     
    m_bars = 1;    
    mflag_bpmChangedThisBar = false;

    m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
    m_pMidiClockPosBeats->forceSet(m_beats);
    m_pMidiClockPosBars->forceSet(m_bars);
}

void MidiClockOut::debugBarTime() {
    m_endTime = std::chrono::steady_clock::now();
    m_barLengthMeasured = std::chrono::duration_cast<std::chrono::microseconds>(m_endTime - m_timeReceivedNewBpm);
    m_timeReceivedNewBpm = m_endTime;
    //qDebug() << "MidiClockOut::tick():BEAT, bpm, tick length(ns):" << m_currentBpm.value() << " , " << m_ticknsTimer.interval();
    if (mflag_bpmChangedThisBar) {
        qDebug() << "MidiClockOut::tick():BAR, bpmChangedThisBar";
    } else {
        m_barLengthError = std::chrono::duration_cast<std::chrono::microseconds>(m_barLengthMeasured - mV_currentTickLength * 96);        
        qDebug() << m_barLengthMeasured << "MidiClockOut::barLengthMeasured";
        qDebug() << mV_currentTickLength * 96 << "MidiClockOut::barLengthTheory";
        qDebug() << "ERROR : " << m_barLengthError;
        qDebug() << "TickLength : " << mV_currentTickLength << ", Error as % of 1 TickLength: " << (double) (m_barLengthError.count() / mV_currentTickLength.count());
        qDebug() << "TICKS in Bar : " << m_debugTickCounter;
        m_debugTickCounter = 0;
    }
    mflag_bpmChangedThisBar = false;
}

bool MidiClockOut::sendDirectRTMidi(uint8_t status) {
    if (m_midiStyleThread) {        
        //return (m_pMidiClockOutThread->sendDirectRTMidi(status));
        return (m_pMidiClockOutThread->queueDirectRTMidi(status));
    }

    QByteArray tickMessage;
    if (status == (uint8_t)0xF8) {
        tickMessage = QByteArray::fromHex("F80000");
    } else if (status == (uint8_t)0xFA) {
        tickMessage = QByteArray::fromHex("FA0000");
    } else if (status == (uint8_t)0xFB) {
        tickMessage = QByteArray::fromHex("FB0000");
    } else if (status == (uint8_t)0xFC) {
        tickMessage = QByteArray::fromHex("FC0000");
    } else {
        return false;
    }

    if (m_pMidiClockOutController) {
        if (m_pMidiClockOutController->isOpen()) {
            return (m_pMidiClockOutController->sendBytes(tickMessage));
        }
    }

    return false;
}
void MidiClockOut::sendMidiClockTick() {    
    // TODO(Tuuli): account for mV_tickAdjustment here to skip ticks
    //m_pMidiClockTick->setParameterFrom(m_tickCount % 16, this);
    sendDirectRTMidi((uint8_t)0xF8);
    //qDebug() << "MidiClockOut::sendMidiClockTick() (0xF8 to portMidi)";
}
void MidiClockOut::sendMidiClockStart() {    
    //m_pMidiClockStart->setParameterFrom(m_tickCount % 16, this);
    sendDirectRTMidi((uint8_t)0xFA);
    qDebug() << "MidiClockOut::sendMidiClockStart() (0xFA to portMidi)";
}
void MidiClockOut::sendMidiClockContinue() {    
    //m_pMidiClockContinue->setParameterFrom(m_tickCount % 16, this);
    sendDirectRTMidi((uint8_t)0xFB);
    qDebug() << "MidiClockOut::sendMidiClockContinue() (0xFB to portMidi)";
}
void MidiClockOut::sendMidiClockStop() {    
    //m_pMidiClockStop->setParameterFrom(m_tickCount % 16, this);
    sendDirectRTMidi((uint8_t)0xFC);
    qDebug() << "MidiClockOut::sendMidiClockStop() (0xFC to portMidi)";
}

void MidiClockOut::adjustSyncTicks(int16_t tickAdjustment) {
    // mV_ticksSinceBpmChange is updated when a tick is sent, and represents number of actually-sent ticks
    // m_tickCount is the GUI's tick counter

    mV_tickAdjustment += tickAdjustment; // Adding lets ticks that havent been sent yet be cancelled, or accumulated.
    qDebug() << "MidiClockOut::adjustSyncTicks " << mV_tickAdjustment;
}
void MidiClockOut::resetQueuedSyncTicks() {
    mV_tickAdjustment = 0;
}

/// @brief Update in the [EngineSync] thread when the BPM is available
/// Called by: 
/// updateLeaderBpm(mixxx::Bpm bpm); 
///    also called from reinitLeaderParams updateInstantaneousBpm slotControlOutEnabled
/// onCallbackStart(); 
/// 
/// Setup a BPM to use for the next tick, and back-project start times to account for mid-tick BPM changes
///  back-projected to where the tick would be had the tempo-change occurred on-beat, so future 
/// ticks stay in-sync. Max drift is the length of the previous tick (5 to 125 ms).
void MidiClockOut::handleNewBPM(mixxx::Bpm newBpm) {            
    auto timeNow = std::chrono::steady_clock::now();     

    if (newBpm.compareEq(m_currentBpm) || !(newBpm.isReasonable())) {                
        return;
    }    
    qDebug() << "MidiClockOut::tick():handleNewBPM" << newBpm;
    
    auto beatPos = m_pMidiClockOutThread->setBeatTimerParameters(timeNow, newBpm.value());

    m_newTickLength = tickLengthFromBpm(newBpm.value());        
    m_currentBpm = newBpm;
    if (m_enabled) {
        // Calculate the % of tick from the current time and the next planned tick
        // percent = duration since last tick / tickLength = (timeNow - timeLastTick) / tickLength

        /// Percent of partial tick time elapsed since last tick
        double tickPercent = (timeNow - (mV_ticksSinceBpmChange * mV_currentTickLength + mV_adjustedTimeReceivedNewBpm)) / mV_currentTickLength;

        // adjustedStartTime = time it would start to be at percent NOW
        // adjustedStartTime + percent * newLength = now
        // adjustedStartTime = now - percent * newLength
        auto tickStartAdjustTime = std::chrono::duration_cast<std::chrono::microseconds>(m_newTickLength * tickPercent);
        mV_adjustedTimeReceivedNewBpm = timeNow - tickStartAdjustTime;

        auto partialIntervalNew = std::chrono::duration_cast<std::chrono::nanoseconds>(m_newTickLength * (1 - tickPercent));
        auto partialIntervalOld = std::chrono::duration_cast<std::chrono::nanoseconds>(mV_currentTickLength * (1 - tickPercent));

        mV_tempoChangeSyncAdjustment += partialIntervalOld - partialIntervalNew; // negative = skip ticks (slower); positive = extra ticks (faster)
    } else {
        mV_adjustedTimeReceivedNewBpm = timeNow;
    }
    /*
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    m_ticknsTimer.setInterval(partialIntervalNew);
#else
    m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(
            partialInterval));
#endif
    m_ticknsTimer.start(); // start a one-shot timer
    m_ticknsTimerID = m_ticknsTimer.id();
    */
    mflag_bpmChangedThisBar = true;
    mV_ticksSinceBpmChange = 0;
}    

void MidiClockOut::tick() {
    // qDebug() << "MidiClockOut::tick()" << mV_ticksSinceBpmChange << " * " << mV_currentTickLength <<  " - " << mV_tempoChangeSyncAdjustment;
    m_debugTickCounter++;
    mV_ticksSinceBpmChange++;

    int32_t syncTicks = 0;
    mV_currentTickLength = m_newTickLength;

    if (m_enabled) {
        auto timeNow = std::chrono::steady_clock::now();
        // qDebug() << "MidiClockOut::tick() ELAPSED " << std::chrono::duration_cast<std::chrono::milliseconds>(timeNow - mV_adjustedTimeReceivedNewBpm);     
        //TODO(Tuuli) mV_adjustedTimeReceivedNewBpm -= mV_tempoChangeSyncAdjustment, and then zero it. Test if its working properly
        m_plannedNextTickTime = mV_adjustedTimeReceivedNewBpm + (mV_currentTickLength * mV_ticksSinceBpmChange) - mV_tempoChangeSyncAdjustment;

        //Account for late ticks by catching up
        auto timeDifference = m_plannedNextTickTime - timeNow;
        // qDebug() << "MidiClockOut::tick() DIFFERENCE " << std::chrono::duration_cast<std::chrono::milliseconds>(timeDifference);
        if (timeDifference > mV_currentTickLength || (timeDifference <= std::chrono::nanoseconds(0))) {
            qDebug() << std::chrono::duration_cast<std::chrono::microseconds>(timeDifference) << "difference (long, or in the past)";
            qDebug() << std::chrono::duration_cast<std::chrono::microseconds>(timeNow.time_since_epoch()) << "now";
            qDebug() << std::chrono::duration_cast<std::chrono::microseconds>(m_plannedNextTickTime.time_since_epoch()) << "planned";
            qDebug() << std::chrono::duration_cast<std::chrono::microseconds>(mV_adjustedTimeReceivedNewBpm.time_since_epoch()) << "recd";
        }
        uint8_t timeout = 0;
        while (m_plannedNextTickTime <= timeNow && (timeout < 100)) {
            m_plannedNextTickTime = mV_adjustedTimeReceivedNewBpm + (mV_currentTickLength * 
                    ++mV_ticksSinceBpmChange) - mV_tempoChangeSyncAdjustment;
            adjustSyncTicks(1);
            timeout++;
            syncTicks++;
        }
        mV_tempoChangeSyncAdjustment = std::chrono::nanoseconds(0);

        //Start next tick timer
        m_intervalLength = std::chrono::duration_cast<std::chrono::nanoseconds>(
                m_plannedNextTickTime - timeNow);
        qDebug() << "MidiClockOut::tick() INTERVAL " << std::chrono::duration_cast<std::chrono::milliseconds>(m_intervalLength) << timeout << syncTicks << "ticks" << mV_ticksSinceBpmChange;
    #if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
                m_intervalLength));
    #else
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(
                m_intervalLength));
    #endif
        m_ticknsTimer.start(); // start a one-shot timer
        m_ticknsTimerID = m_ticknsTimer.id();
    }

    sendMidiClockTick();
    
    bool updateNeeded = false;
    while (syncTicks >= 0) {   
        m_tickCount++;
        if ((m_tickCount % 6) == 0) {
            m_sixteenths++;
            if (((m_sixteenths - 1) % 4) == 0) {
                m_sixteenths = 1;
                m_beats++;
                // qDebug() << "MidiClockOut::tick():BEAT, bpm, tick length(ns):" << m_currentBpm.value() << " , " << m_ticknsTimer.interval();
                if (((m_beats - 1) % 4) == 0) {
                    m_beats = 1;
                    m_bars++;
                    //debugBarTime();
                }
            }
        updateNeeded = true;
        }
    syncTicks--;
    }

    if (updateNeeded) {
        m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
        m_pMidiClockPosBeats->forceSet(m_beats);
        m_pMidiClockPosBars->forceSet(m_bars);
    }    
}

void MidiClockOut::fwdSixteenth() {
    m_tickCount+=6;
    
    adjustSyncTicks(6);

    m_sixteenths++;
    if (((m_sixteenths - 1) % 4) == 0) {
        m_sixteenths = 1;
        m_beats++;
        if (((m_beats - 1) % 4) == 0) {
            m_beats = 1;
            m_bars++;
        }
    }

    m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
    m_pMidiClockPosBeats->forceSet(m_beats);
    m_pMidiClockPosBars->forceSet(m_bars);
    
    qDebug() << "MidiClockOut::fwdSixteenth()";
}

void MidiClockOut::backSixteenth() {
    if (m_tickCount >= 6) {
        adjustSyncTicks(-6);

        m_tickCount -= 6;
        m_sixteenths = ((m_tickCount / 6) % 4) + 1;
        m_beats = ((m_tickCount / 24) % 4) + 1;
        m_bars = (m_tickCount / (24 * 4)) + 1;
    } else {
        sendMidiClockStop();
        if (m_enabled) {
            sendMidiClockStart();
        }              
        m_tickCount = 0;
        m_sixteenths = 1;
        m_beats = 1;
        m_bars = 1;
    }

    m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
    m_pMidiClockPosBeats->forceSet(m_beats);
    m_pMidiClockPosBars->forceSet(m_bars);

    qDebug() << "MidiClockOut::backSixteenth()";
}

std::chrono::microseconds MidiClockOut::getHostTime() const {
    std::chrono::microseconds time =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
    return time;
}

std::chrono::microseconds MidiClockOut::getHostTimeAtSpeaker(std::chrono::microseconds hostTime) const {
    Q_UNUSED(hostTime)
    std::chrono::microseconds time = 
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
    return time;
}

/// This method is called by the [Engine] thread at the start of the audio callback.
/// It captures the current time and updates the audio buffer time.
void MidiClockOut::onCallbackStart(std::chrono::microseconds absTimeWhenPrevOutputBufferReachesDac) {
    m_absTimeWhenPrevOutputBufferReachesDac = absTimeWhenPrevOutputBufferReachesDac;

    if (!m_enabled) {
        return;
    }

    // TODO(Tuuli): Is this necessary? Would checking the sync instead make more sense, or not having checks at all?
    //check leaders bpm 
    /*
    Syncable* target = m_pEngineSync->pickNonSyncSyncTarget(getChannel());
    if (target == nullptr) {
        return;
    }
    auto newSetBpm = target->getBpm();
    if (newSetBpm.isReasonable()) {
        m_newBpm = newSetBpm;
        handleNewBPM();
    }
    */

    // qDebug() << "MidiClockOut::onCallbackStart()" << getHostTime(); // onCallbackStart is being called on the buffer size, for example every 10.7 ms if thats the audio devices buffer size
}

void MidiClockOut::onCallbackEnd(int sampleRate, size_t bufferSize) {
    Q_UNUSED(sampleRate)
    Q_UNUSED(bufferSize)
}