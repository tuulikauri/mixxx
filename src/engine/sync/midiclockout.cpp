//#include "engine/sync/abletonlink.h"

#include <QtDebug>
#include <cmath>

#include "control/controlobject.h"
#include "engine/sync/enginesync.h"
//#include "moc_abletonlink.cpp"
#include "preferences/usersettings.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("MidiClockOut");
constexpr mixxx::Bpm kDefaultBpm(9999.9);
} // namespace

MidiClockOut::MidiClockOut(const QString& group, EngineSync* pEngineSync)
        : m_group(group),
          m_pEngineSync(pEngineSync),
          m_syncMode(SyncMode::None),
          m_oldTempo(kDefaultBpm),
          m_absTimeWhenPrevOutputBufferReachesDac(0),
          m_enabled(false),
          tickCount(0),
          16ths(0),
          beats(0),
          bars(0),
          //m_pLink(std::make_unique<ableton::BasicLink<MixxxClockRef>>(120.0)),
          m_pMidiClockButton(std::make_unique<ControlPushButton>(ConfigKey(group, "out_enabled"))),
          m_pMidiClockPos16ths(std::make_unique<ControlObject>(ConfigKey(group, "num_16th"))),
          m_pMidiClockPosBeats(std::make_unique<ControlObject>(ConfigKey(group, "num_beats"))), 
          m_pMidiClockPosBars(std::make_unique<ControlObject>(ConfigKey(group, "num_bars"))) 

{

    m_pMidiClockButton->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pMidiClockButton->setStates(2);
    QObject::connect(m_pMidiClockButton.get(), 
        &ControlObject::valueChanged, 
        this, 
        &MidiClockOut::slotControlOutEnabled);

    m_pMidiClockPos16ths->setReadOnly();
    m_pMidiClockPos16ths->forceSet(0);

    m_pMidiClockPosBeats->setReadOnly();
    m_pMidiClockPosBeats->forceSet(0);

    m_pMidiClockPosBars->setReadOnly();
    m_pMidiClockPosBars->forceSet(0);

    //audioThreadDebugOutput();
}

MidiClockOut::~MidiClockOut() {    
    m_enabled = false;

    // Disconnect the connection from the Link GUI button
    if (m_pMidiClockButton) {
        QObject::disconnect(m_pMidiClockButton.get(),
                &ControlObject::valueChanged,
                this,
                &MidiClockOut::slotControlOutEnabled);
    }

    // Destroy control objects before releasing Link.
    m_pMidiClockButton.reset();

    // Finally release Link
    //m_pLink.reset();
}

void MidiClockOut::slotControlOutEnabled(double controlButtonValue) {
    m_enabled = (controButtonlValue > 0);
    tick();
}

/// Syncable overrides

/// Notify a Syncable that their mode has changed. The Syncable must record
/// this mode and return the latest mode in response to getMode().
void MidiClockOut::setSyncMode(SyncMode syncMode) {
    m_syncMode = syncMode;
}

/// Notify a Syncable that it is now the only currently-playing syncable.
void MidiClockOut::notifyUniquePlaying() {
}

/// Notify a Syncable that they should sync phase.
void MidiClockOut::requestSync() {
    qDebug() << "MidiClockOut::requestSync()";
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
    return m_enabled;
}

bool MidiClockOut::isQuantized() const {
    return m_enabled;
}

mixxx::Bpm MidiClockOut::getBpm() const {
    return 120; // TODO: dummy bpm
}



void MidiClockOut::testMessage() {
    qDebug() << "MidiClockOut::testMessage()";
}

void MidiClockOut::tick() {
    tickCount++;

    if ((tickCount % 6) == 0) {
        16ths ++;
        if ((16ths % 4) == 0) {
            16ths = 0;
            beats++;
            if ((beats % 4) == 0) {
                beats = 0;
                bars++;
            }
        }
 
    m_pMidiClockPos16th->forceSet(16ths);
    m_pMidiClockPosBeats->forceSet(beats);
    m_pMidiClockPosBars->forceSet(bars);
    }

    qDebug() << "MidiClockOut::tick()";
}
