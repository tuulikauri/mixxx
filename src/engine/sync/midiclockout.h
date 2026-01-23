#include "control/controlpushbutton.h"
#include "engine/channels/enginechannel.h"
#include "engine/enginebuffer.h"
#include "engine/sync/syncable.h"
#include "engine/sync/synccontrol.h"

/// This class manages a Midi clock output (0xF8)

//or std::chrono::microseconds?
using MixxxClockRef = std::chrono::steady_clock; 

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

    void tick();

  private:
    QString m_group;
    EngineSync* m_pEngineSync; // unowned, must outlive this.
    SyncMode m_syncMode;

    mixxx::Bpm m_oldTempo;

    std::chrono::microseconds m_absTimeWhenPrevOutputBufferReachesDac;

    std::unique_ptr<ControlPushButton> m_pMidiClockButton;
    std::unique_ptr<ControlObject> m_pMidiClockPos16ths;
    std::unique_ptr<ControlObject> m_pMidiClockPosBeats;
    std::unique_ptr<ControlObject> m_pMidiClockPosBars;
    
    /// 24PPQN ticks     
    uint32_t tickCount;
    uint8_t 16ths;
    uint8_t beats;
    uint32_t bars;

    /// ControlObject handle for enabling / disabling MidiClockOut pulses
    void slotControlOutEnabled(double controlButtonValue);

    std::chrono::microseconds getHostTime() const;
    std::chrono::microseconds getHostTimeAtSpeaker(std::chrono::microseconds hostTime) const;

    // Test/Debug code

};
