// TODO(Tuuli): Review Controller ownership. Mutexes, semaphors, thread protection? Any shared resources?

// TODO(Tuuli): write tests

// TODO(Tuuli): Check #include scope for .cpp vs .h and move for code quality

// TODO(XXX): Audio playback / bpm is noticeably slower when mixxx isnt the focussed window; no difference with/without MCO enabled

// TODO(Tuuli): midi out methods, 0xF8, and selecting a Midi device in a controller-mapping, controller thread. 
// Would it make sense to use MidiThroughPort, or open a similar virtual port, for MidiClockOut signals?
// Testing qObject->parent() recursion failed; looks like EngineSync doesnt have a parent assigned.
// Using CoreServices to Connect events across threads, and detecting controllers that have Midi Clock Out assigned to send over the pointer to it.
// Make Controller::sendBytes() invokable, or add a slotSendBytes? Should MidiClockOutThread use the Controller resource directly, or via signal/slots that allow messages to be sent in the Controller thread?

// TODO(Tuuli): Should portMidi device have a buffer and timestamps? It might help, for beatjumping ahead especially.. Saw this too... " warning [Controller] PortMidi error: PortMidi: Buffer overflow" is that the output or input buffer? Does the event queue act as a buffer of-sorts for portmidicontroller output?
// TODO(Tuuli) Sequence is vital for MIDI messages; make sure they are never sent out of order. How is this handled? Start-Stop is very different outcome from Stop-Start. Events are always delivered in-order by Qt.
// TODO(XXX) Fix the PortMidiController::sendBytes or Controller MIDI message parser? Is Arduinos MIDI.h open-source compatible with Mixxx, can we use that? Controller::sendBytes doesnt work for non-sysex Midi messages and assumes all messages are sysex... but its the only inherited MIDI sending function that exists in the Controller class. Currently hacked to allow F8, FA, FB, FC

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

    // currently the clock doesnt adopt tempo of a new leader until that leader changes their BPM... fixed now?
    // Need to capture a notice about the new leader and then pull their BPM.

// TODO(Tuuli) BUG/incomplete following tempo changes doesnt work fully yet, it drifts off-beat from tempo changes. Lock to the beat position. Speeds up when tempo is changed.

// Direct access to Controller:
//TODO(Tuuli) BUG Crashing when swapping mapped devices (probably garbage collection / pointer issues) Need to handle updates from ControllerManager similarly to initializing and shutdowns (probably can use the same functions / signals already wired up)
//TODO(Tuuli) BUG Why no messages received by MIDI-OX, was it just that portmidi errored out after a buffer overflow or something? Try to reproduce by crashing / overflowing portmidi and see what happens... Swapping devices seemed to fix it last time...

#include "engine/sync/midiclockout.h"

#include <QtDebug>
#include <cmath>

#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
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

          m_enabled(false),
          m_tickCount(0),
          m_sixteenths(1),
          m_beats(1),
          m_bars(1),

          m_pMidiClockOutThread(std::make_unique<MidiClockOutThread>(this)),
          // GUI objects
          m_pMidiClockEnableButton(std::make_unique<ControlPushButton>(ConfigKey(group, "out_enabled"))),
          m_pMidiClockRestartButton(std::make_unique<ControlPushButton>(ConfigKey(group, "restart"))),
          m_pMidiClockTickButton(std::make_unique<ControlPushButton>(ConfigKey(group, "tick"))),
          m_pMidiClockNudgeFwdButton(std::make_unique<ControlPushButton>(ConfigKey(group, "nudge_fwd"))),
          m_pMidiClockNudgeBackButton(std::make_unique<ControlPushButton>(ConfigKey(group, "nudge_back"))),
          m_pMidiClockPosSixteenths(std::make_unique<ControlObject>(ConfigKey(group, "num_sixteenths"))),
          m_pMidiClockPosBeats(std::make_unique<ControlObject>(ConfigKey(group, "num_beats"))),
          m_pMidiClockPosBars(std::make_unique<ControlObject>(ConfigKey(group, "num_bars"))) {
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

    // Other setup

    m_pMidiClockOutThread->startMidiClockOutThread();
    QObject::connect(m_pMidiClockOutThread.get(), &MidiClockOutThread::tickSent, this, &MidiClockOut::tickGui);

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

    m_pMidiClockEnableButton.reset();
    m_pMidiClockRestartButton.reset();
    m_pMidiClockTickButton.reset();
    m_pMidiClockNudgeFwdButton.reset();
    m_pMidiClockNudgeBackButton.reset();

    m_pMidiClockPosSixteenths.reset();
    m_pMidiClockPosBeats.reset();
    m_pMidiClockPosBars.reset();

    // Destroy pointer safely
    deleteMidiClockOutController();

    qDebug() << "MidiClockOut::destroyed";
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
    qDebug() << "MidiClockOut::slotControlOutEnabled():" << controlButtonValue;
    m_enabled = (controlButtonValue > 0);
    m_pMidiClockOutThread->setBeatClockState(m_enabled);

    if (m_enabled) {
        sendMidiClockStart();
        m_pEngineSync->requestSyncMode(this, SyncMode::Follower);
        //m_pEngineSync->notifyPlayingAudible(this, true); // TODO(Tuuli): is this required? Will we get the leaders tempo if we're audible?
    } else {
        sendMidiClockStop();
        m_pEngineSync->requestSyncMode(this, SyncMode::None);
        //m_pEngineSync->notifyPlayingAudible(this, false);
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
    sendMidiClockTick();
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
    // reinitLeaderParams(pParamsSyncable);
    // pSyncable->updateInstantaneousBpm(pParamsSyncable->getBpm());
    // if (pParamsSyncable != pSyncable && mode != SyncMode::None) pSyncable->requestSync();
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
    auto beatPosition = m_pMidiClockOutThread->getBeatPosAt(std::chrono::steady_clock::now());
    double wholeBeats;
    auto partialBeats = modf(beatPosition, &wholeBeats);
    qWarning() << "MidiClockOut::getBeatDistance() " << partialBeats;
    return partialBeats;
}
mixxx::Bpm MidiClockOut::getBaseBpm() const {
    return m_currentBpm; // TODO(Tuuli): whats this for? Half/double?
}

void MidiClockOut::updateLeaderBeatDistance(double beatDistance) {
    qDebug() << "MidiClockOut::updateLeaderBeatDistance()" << beatDistance;
    // TODO(Tuuli) add a setting or control for toggling following sync changes
    // TODO(Tuuli) We dont store the desired offset between the tick position, and the music's beatDistance; need to get the live Leader_beatDistance at the time of a GUI enable or restart command? Desired offset is the beat 1 location when the DJ sets it... we can probably figure this out / track those intentional "set beat 1 NOW" interaction with MidiClockOut vs the DJ beatjumping the decks...

    /// When the leader moves the beatDistance, follow and queue up a tickSyncAdjustment if the clock is enabled
    if (m_enabled) {
        auto threadSyncOffset = m_pMidiClockOutThread->setBeatPosAt(std::chrono::steady_clock::now(), beatDistance);
        m_pMidiClockOutThread->addPendingSyncAdjustment(threadSyncOffset); // TODO(Tuuli) Add a sync-follow setting
    }
}

void MidiClockOut::forceUpdateLeaderBeatDistance(double beatDistance) {
    qDebug() << "MidiClockOut::forceUpdateLeaderBeatDistance()";
    updateLeaderBeatDistance(beatDistance);
}

// SyncControl::slotRateChanged() (Syncable leader's synccontrol) calls m_pEngineSync->notifyRateChanged(this, bpm / m_leaderBpmAdjustFactor);
// EngineSync::notifyRateChanged calls EngineSync::updateLeaderBpm(pSyncable source, bpm);
//  TODO(Tuuli): should MidiClockOut be one of EngineSync::m_syncables? Probably not; m_syncables is used to choose sync leaders
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
    // EngineChannel* pLeaderChannel = m_pEngineSync->getLeaderChannel();
    // newBeatDistance = pLeaderChannel->getEngineBuffer()->getExactPlayPos();

    // BpmControl::calcSyncAdjustment handles sync.
    // BpmControl::m_dSyncTargetBeatDistance stores the beatDistance
    // BpmControl::setTargetBeatDistance sets the value;
    // called from SyncControl::updateTargetBeatDistance
    // called from EngineBuffer::postProcess; and called from SyncControl::updateLeaderBeatDistance
}

void MidiClockOut::restart() {
    qDebug() << "MidiClockOut::restart(), enabled: " << m_enabled;
    auto beatResetPos = m_pMidiClockOutThread->resetBeatPosAt(std::chrono::steady_clock::now());

    sendMidiClockStop();
    if (m_enabled) {
        sendMidiClockStart();
    }
    resetGui();
}

void MidiClockOut::resetGui() {
    m_sixteenths = 1;
    m_beats = 1;
    m_bars = 1;

    m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
    m_pMidiClockPosBeats->forceSet(m_beats);
    m_pMidiClockPosBars->forceSet(m_bars);
}

bool MidiClockOut::sendDirectRTMidi(uint8_t status) {
    return (m_pMidiClockOutThread->queueDirectRTMidi(status));
}
void MidiClockOut::sendMidiClockTick() {        
    sendDirectRTMidi((uint8_t)0xF8);
    // qDebug() << "MidiClockOut::sendMidiClockTick() (0xF8 to portMidi)";
}
void MidiClockOut::sendMidiClockStart() {    
    sendDirectRTMidi((uint8_t)0xFA);
    //qDebug() << "MidiClockOut::sendMidiClockStart() (0xFA to portMidi)";
}
void MidiClockOut::sendMidiClockContinue() {    
    sendDirectRTMidi((uint8_t)0xFB);
    //qDebug() << "MidiClockOut::sendMidiClockContinue() (0xFB to portMidi)";
}
void MidiClockOut::sendMidiClockStop() {    
    sendDirectRTMidi((uint8_t)0xFC);
    //qDebug() << "MidiClockOut::sendMidiClockStop() (0xFC to portMidi)";
}

/// @brief Update in the [EngineSync] thread when the BPM is available
/// @details Called by:
/// updateLeaderBpm(mixxx::Bpm bpm);
/// also called from reinitLeaderParams updateInstantaneousBpm
void MidiClockOut::handleNewBPM(mixxx::Bpm newBpm) {
    auto timeNow = std::chrono::steady_clock::now();

    if (newBpm.compareEq(m_currentBpm) || !(newBpm.isReasonable())) {
        return;
    }
    qDebug() << "MidiClockOut::handleNewBPM()" << newBpm;
    m_currentBpm = newBpm;
    auto beatPos = m_pMidiClockOutThread->setBeatTempoAt(timeNow, newBpm.value());
}

void MidiClockOut::tickGui(int32_t syncTicks) {
    bool updateNeeded = false;
    while (syncTicks >= 0) {
        m_tickCount++;
        if ((m_tickCount % 6) == 0) {
            m_sixteenths++;
            if (((m_sixteenths - 1) % 4) == 0) {
                m_sixteenths = 1;
                m_beats++;
                // qDebug() << "MidiClockOut::tick():BEAT, bpm " << m_currentBpm.value();
                if (((m_beats - 1) % 4) == 0) {
                    m_beats = 1;
                    m_bars++;
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
    qDebug() << "MidiClockOut::fwdSixteenth()";

    m_pMidiClockOutThread->addPendingSyncAdjustment((int16_t)6);
    return;

    // m_tickCount+=6;
    //
    // adjustSyncTicks(6);

    // m_sixteenths++;
    // if (((m_sixteenths - 1) % 4) == 0) {
    //  m_sixteenths = 1;
    //  m_beats++;
    //  if (((m_beats - 1) % 4) == 0) {
    //  m_beats = 1;
    //  m_bars++;
    //  }
    // }

    // m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
    // m_pMidiClockPosBeats->forceSet(m_beats);
    // m_pMidiClockPosBars->forceSet(m_bars);
}

void MidiClockOut::backSixteenth() {
    qDebug() << "MidiClockOut::backSixteenth()";
    // TODO(Tuuli): dont add negative ticks if they are large or theres no room to back up. Tick-driven sequencers cannot go backwards, so they will pause when the clock is skipping / waiting; meaning silence or perhaps reverb / effects playing out. Label any backwards skips as a PAUSE.
    m_pMidiClockOutThread->addPendingSyncAdjustment((int16_t)(-6));
    return;

    // if (m_tickCount >= 6) {
    //  adjustSyncTicks(-6);

    // m_tickCount -= 6;
    // m_sixteenths = ((m_tickCount / 6) % 4) + 1;
    // m_beats = ((m_tickCount / 24) % 4) + 1;
    // m_bars = (m_tickCount / (24 * 4)) + 1;
    //} else {
    // sendMidiClockStop();
    // if (m_enabled) {
    // sendMidiClockStart();
    // }
    // m_tickCount = 0;
    // m_sixteenths = 1;
    // m_beats = 1;
    // m_bars = 1;
    //}

    // m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
    // m_pMidiClockPosBeats->forceSet(m_beats);
    // m_pMidiClockPosBars->forceSet(m_bars);
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
    // check leaders bpm
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
}

void MidiClockOut::onCallbackEnd(int sampleRate, size_t bufferSize) {
    Q_UNUSED(sampleRate)
    Q_UNUSED(bufferSize)
}
