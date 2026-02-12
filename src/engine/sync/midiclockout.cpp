// TODO(Tuuli): Mutexes, semaphors, thread protection? Any shared resources?

// TODO(Tuuli): Audio playback / bpm is noticeably slower when mixxx isnt the focussed window; no difference with/without MCO enabled

// TODO(Tuuli): FIXED with timestamps and oneshot timers instead::: Ticks are too slow, why? Timer? Could time 96 loops without other stuff happening...?
    // Time 96 ticks and see what the result is
    // Time the beats from the beat_active or beat_distance controls, and see what the timed BPM is

// TODO(Tuuli): midi out, 0xF8, and selecting a Midi device in a controller-mapping, controller thread

// TODO(Tuuli): Should portMidi device have a buffer and timestamps? It might help, for beatjumping ahead especially..

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

// TODO(Tuuli): write tests

// TODO(Tuuli): Not grabbing BPM when another playing syncable becomes leader - fixed now? Test, and remove redundant BPM-hoarding..
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

#include "engine/sync/midiclockout.h"

#include <QtDebug>
#include <cmath>

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
          m_oldTempo(kDefaultBpm),
          m_absTimeWhenPrevOutputBufferReachesDac(0),
          m_enabled(false),
          m_tickCount(0),
          m_tickError(0),
          m_ticksSinceBpmChange(0),
          m_sixteenths(1),
          m_beats(1),
          m_bars(1),
          m_skipNextTick(false),
          mflag_bpmChangedThisBar(false),
          m_currentBpm(kStartBpm),
          m_currentTickLength(kStartTickLength),
          m_maximumNextTickCutoffTime(0),
          m_tickCutOff(ktickCutOff),
          m_ticknsTimerID(Qt::TimerId::Invalid),
          m_debugTickCounter(0),    
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
          m_pMidiClockTick(std::make_unique<ControlObject>(ConfigKey(group, "clock_tick"))),
          m_pMidiClockStart(std::make_unique<ControlObject>(ConfigKey(group, "clock_start"))),
          m_pMidiClockContinue(std::make_unique<ControlObject>(ConfigKey(group, "clock_continue"))),
          m_pMidiClockStop(std::make_unique<ControlObject>(ConfigKey(group, "clock_stop")))
{
    // Setup GUI
    //ControlIndicatorTimer
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
    m_ticknsTimer.callOnTimeout(this, &MidiClockOut::tick);
    m_ticknsTimer.setSingleShot(true);
 
    //audioThreadDebugOutput();

    m_startTime = std::chrono::steady_clock::now();
        
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

    // Destroy control objects before releasing Link.
    m_pMidiClockEnableButton.reset();
    m_pMidiClockRestartButton.reset();
    m_pMidiClockTickButton.reset();
    m_pMidiClockNudgeFwdButton.reset();
    m_pMidiClockNudgeBackButton.reset();
   
    m_pMidiClockPosSixteenths.reset();
    m_pMidiClockPosBeats.reset();
    m_pMidiClockPosBars.reset();
}

// GUI Controls

void MidiClockOut::slotControlOutEnabled(double controlButtonValue) {
    qDebug() << "MidiClockOut::slotControlOutEnabled():" << controlButtonValue;
    m_enabled = (controlButtonValue > 0);
    if (m_enabled) {     
        #if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
                tickLengthFromBpm(m_currentBpm.value())));  
        #else
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(
                tickLengthFromBpm(m_currentBpm.value())));
        #endif
          
        m_ticknsTimer.start();    
        sendMidiClockStart();
        m_ticknsTimerID = m_ticknsTimer.id();
        m_pEngineSync->requestSyncMode(this, SyncMode::Follower);
        m_pEngineSync->notifyPlayingAudible(this, true); //TODO(Tuuli): is this required to grab leaders tempo?        

        updateLeaderBpm(m_currentBpm); // Setup a new BPM change

    } else {
        m_ticknsTimer.stop();
        sendMidiClockStop();
        m_pEngineSync->requestSyncMode(this, SyncMode::None);
    }
}

void MidiClockOut::slotControlRestart(double controlButtonValue) {
    Q_UNUSED(controlButtonValue)
    qDebug() << "MidiClockOut::slotControlRestart()";
    sendMidiClockStop();
    sendMidiClockStart();
    restart();
}

void MidiClockOut::slotControlTick(double controlButtonValue) {  
    Q_UNUSED(controlButtonValue)
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
/// TODO(Tuuli): not Syncable::notifySyncModeChanged?
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
    // TODO(Tuuli): Is this the correct way to get the leaders phase? Syncable::getBaseBpm() and m_pEngineSync->pickNonSyncSyncTarget()
    EngineChannel* pLeaderChannel = m_pEngineSync->getLeaderChannel();
    m_beatDistance = pLeaderChannel->getEngineBuffer()->getExactPlayPos();

    uint32_t newTickCount = m_tickCount - (m_tickCount % 24) + (m_beatDistance.value() * 24); // Replace the partial bar length, 24 ticks per bar
    m_tickError += newTickCount - m_tickCount;
    // TODO(Tuuli): incomplete handling of tickError

    // TODO(Tuuli): should sync tempo as well? Or do we assume this is already handled?
    Syncable* target = m_pEngineSync->pickNonSyncSyncTarget(getChannel());
    if (target == nullptr) {
        return;
    }
    auto newSetBpm = target->getBpm();
    if (newSetBpm.isReasonable()) {
        m_newBpm = newSetBpm;
    }    

    qDebug() << "MidiClockOut::requestSync(), error (ticks):" << m_tickError;
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

    m_tickError = (beatDistance * 24) - (m_tickCount % 24); // TODO(Tuuli): do we want sequencers to keep playing in place, and catch up?
}

void MidiClockOut::forceUpdateLeaderBeatDistance(double beatDistance) {
    qDebug() << "MidiClockOut::forceUpdateLeaderBeatDistance()";
    m_tickCount = m_tickCount - (m_tickCount % 24) + (beatDistance * 24); //TODO(Tuuli): Or jump?
}

//SyncControl::slotRateChanged() (Syncable leader's synccontrol) calls m_pEngineSync->notifyRateChanged(this, bpm / m_leaderBpmAdjustFactor);
//EngineSync::notifyRateChanged calls EngineSync::updateLeaderBpm(pSyncable source, bpm);
// TODO(Tuuli): should MidiClockOut be one of EngineSync::m_syncables? Probably not; syncables is used to choose sync leaders
void MidiClockOut::updateLeaderBpm(mixxx::Bpm bpm) {
    
    // dont follow ultra fast or slow BPMs
    if (!bpm.isReasonable()) {
        return;
    }
    m_newBpm = bpm;    
    m_newTickLength = tickLengthFromBpm(m_newBpm.value());
    m_timeReceivedNewLeaderBpm = std::chrono::duration_cast<std::chrono::microseconds>
            (std::chrono::steady_clock::now().time_since_epoch());
    m_ticksSinceBpmChange = 0;
        
    qDebug() << "MidiClockOut::updateLeaderBpm()" << m_newBpm.value();
    
    /*
    //Spitballing how to get more accuracy when mid-tick tempo change happens.
    
    m_differenceTickLength = m_newTickLength - m_currentTickLength;
    //m_tempoUpdateError = m_differenceTickLength; // TODO(Tuuli): scaled by the position; but probably not achievable?

    m_newNextTickTime = m_plannedNextTickTime + m_differenceTickLength;
    
    if (m_newNextTickTime > m_maximumNextTickCutoffTime) {
        //we set a flag instead of changing anything; this flag might be set
        //many times before the cutoff time
        mflag_useNewInsteadOfPlannedTickTime = true;
        mflag_plannedTickWillBeLate = false;
    } else if (mflag_plannedTickWillBeLate == false) {
        //the next tick is too soon at the new BPM; the old tick will trigger
        //but too late for the new BPM. So catch up on the tick afterwards.
        //Should this timestamp be overwritten if theres another tempo request...
        //or maybe only the first should be saved, since it has the longest time at the
        //old bpm?
        mflag_plannedTickWillBeLate = true;
        m_timeReceivedNewLeaderBpmLate = 
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
    }
    */
}

// TODO(Tuuli): What is this for? Is this function called when a new leader is set?
// This corrects double/half tempo beat rates and resets the rate to the true rate when the Syncable becomes the leader
void MidiClockOut::notifyLeaderParamSource() {
}

void MidiClockOut::reinitLeaderParams(double beatDistance, mixxx::Bpm, mixxx::Bpm bpm) {
    updateLeaderBeatDistance(beatDistance);
    updateLeaderBpm(bpm);
}

void MidiClockOut::updateInstantaneousBpm(mixxx::Bpm bpm) {
    qDebug() << "MidiClockOut::updateInstantaneousBpm()";
    updateLeaderBpm(bpm);
}

/// Class functions

// 24 ppqn tick length in microseconds; mixxx::bpm supports 0 to 500 tempo range
std::chrono::microseconds MidiClockOut::tickLengthFromBpm(double bpm) {
    //[us/t] = 1000000 [us/s] * 60 [s/min] / (24 [t/b] * bpm [b/min]) =
    // 2500000 / bpm   
    qDebug() << "MidiClockOut::tickLengthFromBpm(), bpm:" << bpm;
    std::chrono::microseconds conversion{(int)(2500000 / bpm)};
    return conversion;
}

void MidiClockOut::testMessage() {
    qDebug() << "MidiClockOut::testMessage()";
}

void MidiClockOut::skipTick() {
    qDebug() << "MidiClockOut::skipTick()";
    m_skipNextTick = true;
}

void MidiClockOut::restart() {
    qDebug() << "MidiClockOut::restart()";
    if (m_ticknsTimer.isActive()) {
        m_ticknsTimer.start();
        sendMidiClockStart();
        m_ticknsTimerID = m_ticknsTimer.id();
    } else {
        m_currentBpm = kStartBpm; // TODO(Tuuli): Why reset BPM on restart?
    }
    m_pEngineSync->notifyPlayingAudible(this, true);

    m_tickCount = 0;
    m_tickError = 0;
    m_sixteenths = 1;
    m_beats = 1;     
    m_bars = 1;
    m_skipNextTick = false;
    mflag_bpmChangedThisBar = false;

    m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
    m_pMidiClockPosBeats->forceSet(m_beats);
    m_pMidiClockPosBars->forceSet(m_bars);
}

void MidiClockOut::debugBarTime() {
    m_endTime = std::chrono::steady_clock::now();
    m_barLengthMeasured = std::chrono::duration_cast<std::chrono::microseconds>(m_endTime - m_startTime);
    m_startTime = m_endTime;
    qDebug() << "MidiClockOut::tick():BEAT, bpm, tick length(ns):" << m_currentBpm.value() << " , " << m_ticknsTimer.interval();
    if (mflag_bpmChangedThisBar) {
        qDebug() << "MidiClockOut::tick():BAR, bpmChangedThisBar";
    } else {
        m_barLengthError = std::chrono::duration_cast<std::chrono::microseconds>(m_barLengthMeasured - m_currentTickLength * 96);        
        qDebug() << m_barLengthMeasured << "MidiClockOut::barLengthMeasured";
        qDebug() << m_currentTickLength * 96 << "MidiClockOut::barLengthTheory";
        qDebug() << "ERROR : " << m_barLengthError;
        qDebug() << "TickLength : " << m_currentTickLength << ", Error as % of 1 TickLength: " << (double) m_barLengthError.count() / m_currentTickLength.count();
        qDebug() << "TICKS in Bar : " << m_debugTickCounter;
        m_debugTickCounter = 0;
    }
    mflag_bpmChangedThisBar = false;
}

void MidiClockOut::sendMidiClockTick() {    
    emit clockTick(0, NULL);
    // CoreServices.getControllerManager.controller[i].getMappingScriptFiles.identifier == "midi_clock_out"
    // controller.send(0xF8...)
    m_pMidiClockTick->setParameterFrom(m_tickCount % 16, this);
    qDebug() << "MidiClockOut::sendMidiClockTick() (0xF8 to portMidi)";
}
void MidiClockOut::sendMidiClockStart() {
    emit clockStart(0, NULL);
    m_pMidiClockStart->setParameterFrom(m_tickCount % 16, this);
    qDebug() << "MidiClockOut::sendMidiClockStart() (0xFA to portMidi)";
}
void MidiClockOut::sendMidiClockContinue() {
    emit clockContinue(0, NULL);
    m_pMidiClockContinue->setParameterFrom(m_tickCount % 16, this);
    qDebug() << "MidiClockOut::sendMidiClockContinue() (0xFB to portMidi)";
}
void MidiClockOut::sendMidiClockStop() {
    emit clockStop(0, NULL);
    m_pMidiClockStop->setParameterFrom(m_tickCount % 16, this);
    qDebug() << "MidiClockOut::sendMidiClockStop() (0xFC to portMidi)";
}

void MidiClockOut::tick() {
    //qDebug() << "MidiClockOut::tick():";
    m_debugTickCounter++;

    // Handle tick error correction
    if (m_tickError > 1) {
        // TODO(Tuuli): Do something to catchup with the timer for each tick until its fixed; shorter timers so the 0xF8s
        // are still sent, but quicker for a few pulses until caught up
        // Or... just send them all at once and let the serial buffer / MIDI buffer handle it? Brrrrrrrrrrr
    } else if (m_tickError < 0) {
        qDebug() << "MidiClockOut::tick():SKIP";
        m_skipNextTick = true;
        // TODO(Tuuli): Check on positives/ negatives... so that the error pushes the sync in the right direction
    }

    // Handle change to BPM
    if (!m_newBpm.compareEq(m_currentBpm) && m_newBpm.isReasonable()) {
        qDebug() << "MidiClockOut::tick():newbpm";
        
        // TODO(Tuuli): Handle mid-tick error in BPM change, to bring the ticks into their bar position
        
        m_currentBpm = m_newBpm;
        m_currentTickLength = tickLengthFromBpm(m_currentBpm.value());

        #if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
                m_currentTickLength));
        #else
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(
                m_currentTickLength));
        #endif
        
        m_ticknsTimer.start();        
        m_ticknsTimerID = m_ticknsTimer.id();    
        mflag_bpmChangedThisBar = true;
    }

    // Handle skipping (or TODO(Tuuli) extra ticks)
    if (m_skipNextTick) {
        qDebug() << "MidiClockOut::tick():skipNextTick";
        m_skipNextTick = false;
        return;
    }

    // m_plannedNextTickTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch() + m_currentTickLength);
    m_maximumNextTickCutoffTime = m_plannedNextTickTime - m_tickCutOff;

    m_ticksSinceBpmChange++;
    m_plannedNextTickTime = m_timeReceivedNewLeaderBpm + m_currentTickLength * m_ticksSinceBpmChange; 
    m_intervalLength = std::chrono::duration_cast<std::chrono::nanoseconds>(
            m_plannedNextTickTime - std::chrono::steady_clock::now().time_since_epoch());
    // m_intervalLength = m_plannedNextTickTime - now();
    // m_intervalLength = (m_timeReceivedNewLeaderBpm + L*n) - (m_timeReceivedNewLeaderBpm + L*(n-1));
    // m_intervalLength = (L*n) - (L*n-L));
    // m_intervalLength = L; // in a perfect timing world...

    // Handle late ticks
    if (m_intervalLength.count() > 0) {
        #if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::nanoseconds>(
                m_intervalLength));
        #else
        m_ticknsTimer.setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(
                m_intervalLength));
        #endif        
        m_ticknsTimer.start(); // start a one-shot timer
        m_ticknsTimerID = m_ticknsTimer.id();
    } else {
        tick(); // TODO(Tuuli): Does recursion work here? Any risk it doesnt converge? This should run up m_ticksSinceBpmChange until its > 0
    }
    
    sendMidiClockTick();

    m_tickCount++;

    if ((m_tickCount % 6) == 0) {
        m_sixteenths++;
        if (((m_sixteenths - 1) % 4) == 0) {
            m_sixteenths = 1;
            m_beats++;            
            //qDebug() << "MidiClockOut::tick():BEAT, bpm, tick length(ns):" << m_currentBpm.value() << " , " << m_ticknsTimer.interval();
            if (((m_beats - 1) % 4) == 0) {
                m_beats = 1;
                m_bars++;
                debugBarTime();
            }
        }
 
    m_pMidiClockPosSixteenths->forceSet(m_sixteenths);
    m_pMidiClockPosBeats->forceSet(m_beats);
    m_pMidiClockPosBars->forceSet(m_bars);
    }
}

void MidiClockOut::fwdSixteenth() {
    m_tickCount+=6;

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
        m_tickCount -= 6;
        m_sixteenths = ((m_tickCount / 6) % 4) + 1;
        m_beats = ((m_tickCount / 24) % 4) + 1;
        m_bars = (m_tickCount / (24 * 4)) + 1;
    } else {
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
    std::chrono::microseconds time = 
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
    return time;
}

/// This method is called at the start of the audio callback, approximately every 10 ms.
/// It captures the current time and updates the audio buffer time.
void MidiClockOut::onCallbackStart(std::chrono::microseconds absTimeWhenPrevOutputBufferReachesDac) {
    m_absTimeWhenPrevOutputBufferReachesDac = absTimeWhenPrevOutputBufferReachesDac;

    if (!m_enabled) {
        return;
    }

    //check leaders bpm
    Syncable* target = m_pEngineSync->pickNonSyncSyncTarget(getChannel());
    if (target == nullptr) {
        return;
    }
    auto newSetBpm = target->getBpm();
    if (newSetBpm.isReasonable()) {
        m_newBpm = newSetBpm;
    }

    // qDebug() << "MidiClockOut::onCallbackStart()" << getHostTime(); // onCallbackStart is being called about every 10ms
    
}

void MidiClockOut::onCallbackEnd(int sampleRate, size_t bufferSize) {
    Q_UNUSED(sampleRate)
    Q_UNUSED(bufferSize)
}