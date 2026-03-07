#include "engine/sync/midiclockoutthread.h"
#include "engine/sync/midiclockout.h"

#include <QtDebug>

#include "moc_midiclockoutthread.cpp"
#include "preferences/usersettings.h"
#include "util/logger.h"
#include "util/runtimeloggingcategory.h"
#include "util/string.h"
#include "util/time.h"
#include "util/trace.h"

namespace {
const mixxx::Logger kLogger("MidiClockOutThread");
constexpr std::chrono::microseconds kStartTickLength{(int)(2500000.0 / 120.0)};
constexpr double kStartBeatSpeed{(120.0 / 60'000'000.0)};
}

MidiClockOutThread::MidiClockOutThread(MidiClockOut* parent) :
          QThread(parent),
          m_pMidiClockOutParent(parent),
          stopplz(false),
          m_midiFIFOQueue(512),
          m_beatClockRunning(false),
          m_beatSpeed(kStartBeatSpeed),
          m_beatStartPos(0),
          m_nextBeatPos(0),
          m_tickCount(0),
          m_tickSyncAdjustment(0) {
    this->setObjectName("MidiClockOutThread");
    m_pMidiFIFOQueue = &m_midiFIFOQueue;
    m_beatStartTime = std::chrono::steady_clock::now();
    qDebug() << "MidiClockOutThread::Created ";
}
MidiClockOutThread::~MidiClockOutThread() {
    qDebug() << "MidiClockOutThread::~MidiClockOutThread() destroying ";
    mutex.lock();
    stopplz = true;
    cond.wakeOne(); //TODO(Tuuli) remove this cond
    condMidiControllerExists.wakeOne();
    mutex.unlock();
    qDebug() << "MidiClockOutThread::~MidiClockOutThread() waiting ";
    wait();
    qDebug() << "MidiClockOutThread::~MidiClockOutThread() done ";
}
void MidiClockOutThread::startMidiClockOutThread() { // TODO(Tuuli) This can be combined with setMidiClockOutController; theres no reason for the thread to run if there's no MIDI controller mapped
    qDebug() << "MidiClockOutThread::startMidiClockOutThread()";
    QMutexLocker locker(&mutex);
    if (!isRunning()) {
        // m_startTime = std::chrono::steady_clock::now();
        start(QThread::HighestPriority);
    } else {
        if (m_pMidiClockOutController) {
            qDebug() << "MidiClockOutThread::startMidiClockOutThread() Wake up, theres a midi controller";
            condMidiControllerExists.wakeOne();
        }
        cond.wakeOne();
    qDebug() << "MidiClockOutThread::startMidiClockOutThread";
    }
}
void MidiClockOutThread::stopThreadAndWait() {
    mutex.lock();
    stopplz = true;
    mutex.unlock();

    resetPendingSyncAdjustment();

    midiMutex.lock();
    m_pMidiFIFOQueue->releaseReadRegions(m_pMidiFIFOQueue->readAvailable());
    midiMutex.unlock();

    mutex.lock();
    stopplz = true;
    condMidiControllerExists.wakeOne();
    cond.wakeOne();
    mutex.unlock();

    wait();
}
void MidiClockOutThread::testuSleepLength() {
    auto startTime = std::chrono::steady_clock::now();
    uint32_t ticks = 0;
    bool stopNow = false;
    while (!stopNow && ticks < 5000) {
        ticks++;
        usleep(200);

        mutex.lock();
        stopNow = stopplz;
        mutex.unlock();
    }
    auto endTime = std::chrono::steady_clock::now();
    qDebug() << "MidiClockOutThread::testuSleepLength (5000*0.2ms = 10s) " << std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
}

bool MidiClockOutThread::queueDirectRTMidi(uint8_t status) {
    const QMutexLocker locker(&midiMutex);
    if (m_pMidiFIFOQueue->write(&status, 1) == 1) {
        //qDebug() << "MidiClockOutThread::queueDirectRTMidi" << status;
        return true;
    }
    return false;
}
uint16_t MidiClockOutThread::queueDirectRTMidiMultiple(uint8_t status, int count) {
    uint16_t written = 0;
    const QMutexLocker locker(&midiMutex);
    while (m_pMidiFIFOQueue->write(&status, 1) == 1 && (written < count) && (written < 288)) {
        // qDebug() << "MidiClockOutThread::queueDirectRTMidiMultiple, queued " << status;
        written++;
    }
    if (written == (count - 1)) {
        return 0;
    }
    return written;
}
bool MidiClockOutThread::sendDirectRTMidi(uint8_t status) {
    QByteArray midiRTMessage;    
    if (status == (uint8_t)0xF8) {
        midiRTMessage = QByteArray::fromHex("F80000");
    } else if (status == (uint8_t)0xFA) {
        midiRTMessage = QByteArray::fromHex("FA0000");
    } else if (status == (uint8_t)0xFB) {
        midiRTMessage = QByteArray::fromHex("FB0000");
    } else if (status == (uint8_t)0xFC) {
        midiRTMessage = QByteArray::fromHex("FC0000");
    } else {
        return false;
    }
    
    mutex.lock();
    if (m_pMidiClockOutController) {
        if (m_pMidiClockOutController->isOpen()) {
            auto midiResult = m_pMidiClockOutController->sendBytes(midiRTMessage);
            mutex.unlock();
            return (midiResult); // TODO(Tuuli) Eventually calls portmidi.h::Pm_WriteShort(m_pStream, 0, message) - is this blocking?
        }
    }
    mutex.unlock();

    //qDebug() << "MidiClockOutThread::sendDirectRTMidi, sent " << status;
    return false;
}
void MidiClockOutThread::setMidiClockOutController(Controller* pMidiClockOutController) {
    qDebug() << "MidiClockOutThread::setMidiClockOutController()";
    mutex.lock();
    m_pMidiClockOutController = pMidiClockOutController;
    mutex.unlock();

    /// When a new controller is linked, reset any beats that accumulated previously. The tempo that has been tracked remains as the base tempo, so that external sequencers start at the most recent tempo
    resetBeatPosAt(std::chrono::steady_clock::now());
    startMidiClockOutThread();
}
void MidiClockOutThread::deleteMidiClockOutController() {
    qDebug() << "MidiClockOutThread::deleteMidiClockOutController()";
    mutex.lock();
    m_pMidiClockOutController = nullptr;
    mutex.unlock();

    resetPendingSyncAdjustment();

    midiMutex.lock();
    m_pMidiFIFOQueue->releaseReadRegions(m_pMidiFIFOQueue->readAvailable());
    midiMutex.unlock();
}

void MidiClockOutThread::setBeatClockState(bool state) {
    if (state) {
        resetBeatPosAt(std::chrono::steady_clock::now());
    }
    const QMutexLocker locker(&beatMutex);
    m_beatClockRunning = state;
}
bool MidiClockOutThread::getBeatClockState() {
    const QMutexLocker locker(&beatMutex);
    return m_beatClockRunning;
}

double MidiClockOutThread::calcBeatSpeedFromBpm(double bpm) {
    return bpm / 60'000'000.0;
}
double MidiClockOutThread::calcNextTickBeatPos(double beatPosition) {
    uint32_t ticksPassed = std::floor(beatPosition * 24.0);
    return ((double)(ticksPassed+1) / 24.0);
}
double MidiClockOutThread::calcPrevTickBeatPos(double beatPosition) {
    uint32_t ticksPassed = std::floor(beatPosition * 24.0);
    return ((double)(ticksPassed) / 24.0);
}
int32_t MidiClockOutThread::calcTicksBetween(double beatPositionStart, double beatPositionEnd) {
    return std::floor((beatPositionEnd - beatPositionStart) * 24.0); // TODO(Tuuli): Does floor make sense? Might be a negative number. How do we want it rounded? Towards zero? Up/down?
}
int32_t MidiClockOutThread::calcTicksFromBeatPos(double beatPosition) {
    return std::floor(beatPosition * 24.0); // TODO(Tuuli): Does floor make sense? Might be a negative number. How do we want it rounded? Towards zero? Up/down?
}

double MidiClockOutThread::getBeatPosAt(std::chrono::steady_clock::time_point time) {
    const QMutexLocker locker(&beatMutex);
    return m_beatStartPos + m_beatSpeed * ((time - m_beatStartTime) / std::chrono::microseconds(1));
}
double MidiClockOutThread::setBeatTempoAt(std::chrono::steady_clock::time_point startTime, double bpm) {
    double beatSpeed = calcBeatSpeedFromBpm(bpm);
    double beatStartPos = getBeatPosAt(startTime); /// The previous beat location at that time stamp is now the starting beat position for the new speed, with the new start time

    const QMutexLocker locker(&beatMutex);
    m_beatSpeed = beatSpeed;
    m_beatStartTime = startTime;
    m_beatStartPos = beatStartPos;
    return beatStartPos;
}
double MidiClockOutThread::setBeatPosAt(std::chrono::steady_clock::time_point time, double beatPos, bool addExisting) {
    int numBeats = 0;
    double beatStartPos = getBeatPosAt(time);
    if (addExisting) {
        numBeats = std::floor(beatStartPos);
    }

    auto newStart = beatPos + numBeats; /// Add whole beats to avoid jumping the count
    // next beat is the next one from now(), not from time - this is wrong probably...
    auto timeNow = std::chrono::steady_clock::now();
    //auto currentPos = getBeatPosAt(timeNow);
    //auto nextCurrentBeat = calcNextTickBeatPos(currentPos); //in the old beatPos

    //auto newCurrentPos = getBeatPosAt(std::chrono::steady_clock::now());
    //auto newCurrentPos = m_beatStartPos + m_beatSpeed * ((time - m_beatStartTime) / std::chrono::microseconds(1));
    //auto newCurrentPos = newStart + m_beatSpeed * ((timeNow - time) / std::chrono::microseconds(1));
    //auto newNextBeat = calcNextTickBeatPos(newCurrentPos);

    //in the previous run() loop.. before jumping
    //m_nextBeatPos = calcNextTickBeatPos(m_tickCount / 24.0);
    //m_tickCount++;
    

    const QMutexLocker locker(&beatMutex);
    /////////////////// logic review...
    qWarning() << "DEBUG: MidiClockOutThread::setBeatPosAt() " << beatPos << beatStartPos << newStart << m_beatStartPos;
    //auto oldStartatZero = m_beatSpeed * ((time - m_beatStartTime) / std::chrono::microseconds(1));
    //auto newStartatZero = m_beatSpeed * ((time - time) / std::chrono::microseconds(1));
    //newStartatZero - oldStartatZero = m_beatSpeed * (m_beatStartTime - time) / std::chrono::microseconds(1));

    //auto oldPosatTime = beatStartPos = m_beatStartPos + m_beatSpeed * ((time - m_beatStartTime) / std::chrono::microseconds(1));
    //auto newPosatTime = newStart = beatPos + numBeats;
    //auto shift = newPosatTime - oldPosatTime;
    ///////////////////


    m_beatStartPos = newStart;
    m_beatStartTime = time;
    ////m_nextBeatPos = nextBeat;
    ////m_tickCount = std::floor(currentPos * 24.0);
    //auto newCurrentPos = getBeatPosAt(std::chrono::steady_clock::now());
    auto newCurrentPos = newStart + m_beatSpeed * ((timeNow - time) / std::chrono::microseconds(1));
    m_nextBeatPos = calcNextTickBeatPos(newCurrentPos);
    m_tickCount = calcTicksFromBeatPos(newCurrentPos);

    return (m_beatStartPos - beatStartPos);
    //beatPos - (beatStartPos - floor(beatStartPos))
}
double MidiClockOutThread::setBeatPosFromBeatDistanceAt(std::chrono::steady_clock::time_point time, double beatDistance, bool addExisting) {
    qWarning() << "DEBUG: MidiClockOutThread::setBeatPosFromBeatDistanceAt " << beatDistance;
    double wholeBeats;
    auto partialBeats = modf(beatDistance, &wholeBeats);
    //return setBeatPosAt(time, partialBeats, addExisting); // TODO(Tuuli) This isnt working yet, so return 0
    return 0;
}
double MidiClockOutThread::resetBeatPosAt(std::chrono::steady_clock::time_point time) {    
    double beatResetPos = getBeatPosAt(time); /// The previous beat location at that time stamp is now the starting beat location

    const QMutexLocker locker(&beatMutex);
    m_beatStartTime = time;
    m_beatStartPos = 0;
    m_tickCount = 0;
    m_nextBeatPos = 0;
    return beatResetPos;
}
double MidiClockOutThread::getNextBeatPos() {
    const QMutexLocker locker(&beatMutex);
    return m_nextBeatPos;
}

void MidiClockOutThread::addPendingSyncAdjustment(int16_t syncShift) {
    qDebug() << "MidiClockOutThread::addPendingSyncAdjustment(int) " << syncShift;
    const QMutexLocker locker(&beatMutex);
    m_tickSyncAdjustment = (m_tickSyncAdjustment + syncShift) % 288; /// Limits the pending sync range to 12 beats (3 bars at 4/4, 4 bars at 3/4) (288 ticks)
}
void MidiClockOutThread::addPendingSyncAdjustment(double syncShift) {
    qDebug() << "MidiClockOutThread::addPendingSyncAdjustment(double) " << syncShift;

    const QMutexLocker locker(&beatMutex);
    m_tickSyncAdjustment = (m_tickSyncAdjustment + calcTicksFromBeatPos(syncShift)) % 288;
}
int16_t MidiClockOutThread::resetPendingSyncAdjustment() {
    const QMutexLocker locker(&beatMutex);
    auto removePendingAmount = m_tickSyncAdjustment;
    m_tickSyncAdjustment = 0;
    return removePendingAmount;
}
int16_t MidiClockOutThread::getPendingSyncAdjustment() {
    const QMutexLocker locker(&beatMutex);
    //if (m_tickSyncAdjustment != 0) {
    //    qDebug() << "MidiClockOutThread, Sync ticks: " << m_tickSyncAdjustment;
    //}
    return m_tickSyncAdjustment;
}


void MidiClockOutThread::run() {
    qDebug() << "MidiClockOutThread::run() start";
    bool stopNow = false;

    beatMutex.lock();
    m_tickCount = 0; 
    m_nextBeatPos = 0;
    m_tickSyncAdjustment = 0;
    beatMutex.unlock();

    std::chrono::time_point nowTime = std::chrono::steady_clock::now();

    while (!stopNow) {
        /// Add extra sync tick to the queue
        if (getPendingSyncAdjustment() > 0) { 
            if (!queueDirectRTMidi(0xF8)) {
                qWarning() << "Failed to queue extra tick";
            } else {
                addPendingSyncAdjustment((int16_t)(-1));
                qDebug() << "MidiClockOutThread, Queued extra tick ";
            }
        }

        /// Beat clock - generates time-based ticks to put in the queue
        nowTime = std::chrono::steady_clock::now();
        auto currentBeatPos = getBeatPosAt(nowTime);
        if (getBeatClockState()) {
            auto nextPos = getNextBeatPos();
            //qDebug() << "MidiClockOutThread::run() Current,next: " << currentBeatPos << nextPos;
            if (currentBeatPos > getNextBeatPos()) {
                //qDebug() << "TIME FOR NO MORE OF THOSE THREAD-BLOCKIN' BEATS!";
                if (!queueDirectRTMidi(0xF8)) {
                    qWarning() << "Failed to queue";
                }
                beatMutex.lock();
                // This factors out to m_nextBeatPos = (m_tickCount + 1) / 24.0;
                m_nextBeatPos = calcNextTickBeatPos(m_tickCount / 24.0); // TODO(Tuuli): check old commits before thread, ++mV_ticksSinceBpmChange in tick() for how planned ticks in the past are handled without the thread
                m_tickCount++;
                beatMutex.unlock();
            }
        }

        /// Midi
        midiMutex.lock();
        if (m_pMidiFIFOQueue->readAvailable()) {
            uint8_t data;
            if(m_pMidiFIFOQueue->read(&data, 1) == 1){
                midiMutex.unlock();
                if ((data == 0xF8) && (getPendingSyncAdjustment() < 0)) {
                    addPendingSyncAdjustment((int16_t)1);
                    qDebug() << "MidiClockOutThread, Skipped a tick ";
                } else {
                    if (sendDirectRTMidi(data)) {
                        // qDebug() << "MidiClockOutThread::sendDirectRTMidi, sent " << data;
                        if (data == 0xF8) {
                            emit tickSent(0);
                        }
                    } else {
                        qWarning() << "MidiClockOutThread::run()::sendDirectRTMidi, failed to send " << data; // TODO(Tuuli) This warning is posted about 50 times on exit when trying to use emit deleteMidiClockOut() in ControllerManager, why?
                    }
                }
            } else {
                midiMutex.unlock();
                qWarning() << "MidiClockOutThread::run(), failed to read FIFO buffer but it has data ";
            }
        } else {
            midiMutex.unlock();
        }

        usleep(200);

        mutex.lock(); //TODO(Tuuli) How expensive is this mutex lock? This could be a shared mutex since it is read every 200us but written rarely. Or an atomic boolean variable?
        stopNow = stopplz;
        /// If there's no longer a Midi controller, but the thread hasn't been told to end, wait for a new Midi controller
        // TODO(Tuuli) This isnt exiting properly as Controller Manager is deleted; it doesnt receive the shutdown signals if they are emitted by ControllerManager and still tried to send Midi to a deleted Controller, until EngineSync is deleted. Instead, calling m_pEngine->slotDeleteMidiClockOut(""); from CoreServices::finalize();
        if (!m_pMidiClockOutController && !stopNow) {
            qDebug() << "MidiClockOutThread::run() Sleeping now... zzzzzzz ";
            condMidiControllerExists.wait(&mutex);
            qDebug() << "MidiClockOutThread::run() Waking up now... Hello?? ";
            stopNow = stopplz;
        }
        mutex.unlock();
    }
}
