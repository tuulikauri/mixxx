#pragma once
#include <ableton/platforms/stl/Clock.hpp>
#include <QTimer>
#include <QChronoTimer>
#include "control/controlpushbutton.h"
#include "engine/channels/enginechannel.h"
#include "engine/enginebuffer.h"
#include "engine/sync/syncable.h"
#include "engine/sync/synccontrol.h"

/// This class manages a Midi clock output (0xF8)

//or std::chrono::microseconds?
//using MixxxClockRef = std::chrono::steady_clock; 
using MixxxClockRef = ableton::platforms::stl::Clock;

class MidiClockOut : public QObject, public Syncable {
  Q_OBJECT
  public:
    MidiClockOut(const QString& group, EngineSync* pEngineSync);
    ~MidiClockOut() override;

    const QString& getGroup() const override {
        return m_group;
    }
    EngineChannel* getChannel() const override {
        return nullptr;
    }
   

    /// Notify a Syncable that their mode has changed. The Syncable must record
    /// this mode and return the latest mode in response to getMode().
    void setSyncMode(SyncMode mode) override;

    /// Notify a Syncable that it is now the only currently-playing syncable.
    void notifyUniquePlaying() override;

    /// Notify a Syncable that they should sync phase.
    void requestSync() override;

    /// Must NEVER return a mode that was not set directly via
    /// notifySyncModeChanged.
    SyncMode getSyncMode() const override;

    /// Only relevant for player Syncables.
    bool isPlaying() const override;
    bool isAudible() const override;
    bool isQuantized() const override;

    /// Gets the current speed of the syncable in bpm (bpm * rate slider), doesn't
    /// include scratch or FF/REW values.
    mixxx::Bpm getBpm() const override;

    
    /// Gets the beat distance as a fraction from 0 to 1
    double getBeatDistance() const override;

    /// Gets the speed of the syncable if it was playing at 1.0 rate.
    mixxx::Bpm getBaseBpm() const override;

    /// The following functions are used to tell syncables about the state of the
    /// current Sync Master.
    /// Must never result in a call to
    /// SyncableListener::notifyBeatDistanceChanged or signal loops could occur.
    void updateLeaderBeatDistance(double beatDistance) override;

    /// Enforces the immediate change of the beat distance of all Link peers
    void forceUpdateLeaderBeatDistance(double beatDistance);

    /// Must never result in a call to SyncableListener::notifyBpmChanged or
    /// signal loops could occur.
    void updateLeaderBpm(mixxx::Bpm bpm) override;

    void notifyLeaderParamSource() override;

    /// Combines the above three calls into one, since they are often set
    /// simultaneously.  Avoids redundant recalculation that would occur by
    /// using the three calls separately.
    void reinitLeaderParams(double beatDistance, mixxx::Bpm baseBpm, mixxx::Bpm bpm) override;

    /// Must never result in a call to
    /// SyncableListener::notifyInstantaneousBpmChanged or signal loops could
    /// occur.
    void updateInstantaneousBpm(mixxx::Bpm bpm) override;

    void onCallbackStart(std::chrono::microseconds absTimeWhenPrevOutputBufferReachesDac);
    void onCallbackEnd(int sampleRate, size_t bufferSize);

    void testMessage();

    void tick();
    void backSixteenth();
    void fwdSixteenth();
  
  private slots:    
    void slotControlOutEnabled(double controlButtonValue);
    void slotControlRestart(double controlButtonValue);
    void slotControlTick(double controlButtonValue);
    void slotControlNudgeFwd(double controlButtonValue);
    void slotControlNudgeBack(double controlButtonValue);

  private:
    // ableton::link::HostTimeFilter<MixxxClockRef> m_hostTimeFilter;

    QChronoTimer ticknsTimer = QChronoTimer(nullptr);
    Qt::TimerId ticknsTimerID;

    QString m_group;
    EngineSync* m_pEngineSync; // unowned, must outlive this.
    SyncMode m_syncMode;

    mixxx::Bpm m_oldTempo;
    mixxx::Bpm currentBpm;
    mixxx::Bpm newBpm;
    double dnewBpm;


    std::chrono::microseconds m_absTimeWhenPrevOutputBufferReachesDac;
    std::chrono::microseconds nextTickTime;
    std::chrono::microseconds plannedNextTickTime;    
    std::chrono::microseconds newNextTickTime;
    std::chrono::microseconds differenceTickLength;

    std::chrono::microseconds timeReceivedNewLeaderBpmLate;    
    std::chrono::microseconds maximumNextTickCutoffTime;
    

    std::chrono::microseconds tickLengthFromBpm(double bpm);
    std::chrono::microseconds currentTickLength;
    std::chrono::microseconds newTickLength;
    std::chrono::microseconds tickCutOff;    

    mixxx::audio::FramePos beatDistance;

    bool flag_plannedTickWillBeLate;
    bool flag_useNewInsteadOfPlannedTickTime;
    bool flag_bpmChangedThisBar;

    bool enabled;   

    /// 24PPQN ticks     
    uint32_t tickCount;
    uint8_t sixteenths;
    uint8_t beats;
    uint32_t bars;
    int32_t tickError;

    bool skipNextTick;    

    void skipTick();

    //Restart all tick counters, all bpm adjusters, and the tick clock (if its running)
    void restart();

    //Control objects
    std::unique_ptr<ControlPushButton> m_pMidiClockEnableButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockRestartButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockTickButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockNudgeFwdButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockNudgeBackButton;
    std::unique_ptr<ControlObject> m_pMidiClockPosSixteenths;
    std::unique_ptr<ControlObject> m_pMidiClockPosBeats;
    std::unique_ptr<ControlObject> m_pMidiClockPosBars;

    std::chrono::microseconds getHostTime() const;
    std::chrono::microseconds getHostTimeAtSpeaker(std::chrono::microseconds hostTime) const;

    // Test/Debug code

    std::chrono::microseconds barLengthMeasured;
    std::chrono::microseconds barLengthError;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point endTime;

    uint32_t debugTickCounter;

    void debugBarTime();
    };
