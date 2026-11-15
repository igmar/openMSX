#include "EmuTimer.hh"

#include "serialize.hh"

#include <memory>

namespace openmsx {

std::unique_ptr<EmuTimer> EmuTimer::createOPM_1(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 1, 3579545, 64 * 2, 1024);
}

std::unique_ptr<EmuTimer> EmuTimer::createOPM_2(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 2, 3579545, 64 * 2 * 16, 256);
}

std::unique_ptr<EmuTimer> EmuTimer::createOPP_2(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 2, 3579545, 64 * 2 * 32, 256);
}

std::unique_ptr<EmuTimer> EmuTimer::createOPL3_1(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 0x40, 3579545, 72 * 4, 256);
}

std::unique_ptr<EmuTimer> EmuTimer::createOPL3_2(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 0x20, 3579545, 72 * 4 * 4, 256);
}

// The Y8950's timers are the OPL3's, but their ticks come from a prescaler
// that runs freely: a start write does not restart it. Measured on a real
// NMS 1205: with timer 1 = FFh (one tick), the time from the start write to
// the flag is a sawtooth over the start's phase, between about 3 and 14
// polls of 25 T, with a period of one tick.
std::unique_ptr<EmuTimer> EmuTimer::createY8950_1(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 0x40, 3579545, 72 * 4, 256, true);
}

std::unique_ptr<EmuTimer> EmuTimer::createY8950_2(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 0x20, 3579545, 72 * 4 * 4, 256, true);
}

std::unique_ptr<EmuTimer> EmuTimer::createOPL4_1(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 0x40, 33868800, 72 * 38, 256);
}

std::unique_ptr<EmuTimer> EmuTimer::createOPL4_2(
	Scheduler& scheduler, EmuTimerCallback& cb)
{
	return std::make_unique<EmuTimer>(
		scheduler, cb, 0x20, 33868800, 72 * 38 * 4, 256);
}


EmuTimer::EmuTimer(Scheduler& scheduler_, EmuTimerCallback& cb_,
                   uint8_t flag_, unsigned freq_num, unsigned freq_denom,
                   int maxVal_, bool freeRunning_)
	: Schedulable(scheduler_), cb(cb_)
	, maxVal(maxVal_), count(maxVal_)
	, flag(flag_), freeRunning(freeRunning_)
{
	clock.setFreq(freq_num, freq_denom);
	prescaler.setFreq(freq_num, freq_denom);
}

void EmuTimer::setValue(int value)
{
	count = maxVal - value;
}

void EmuTimer::setStart(bool start, EmuTime time)
{
	if (start != counting) {
		counting = start;
		if (start) {
			schedule(time);
		} else {
			unschedule();
		}
	}
}

void EmuTimer::schedule(EmuTime time)
{
	if (freeRunning) {
		// Count from the prescaler's last tick at or before 'time': the
		// first period after a start write is between (count - 1) and
		// count ticks long, the later ones exactly count ticks. The
		// prescaler's ticks lie on a fixed grid from time zero.
		prescaler.advance(time);
		clock.reset(prescaler.getTime());
	} else {
		clock.reset(time);
	}
	clock += count;
	setSyncPoint(clock.getTime());
}

void EmuTimer::unschedule()
{
	removeSyncPoint();
}

void EmuTimer::executeUntil(EmuTime time)
{
	cb.callback(flag);
	schedule(time);
}

template<typename Archive>
void EmuTimer::serialize(Archive& ar, unsigned /*version*/)
{
	ar.template serializeBase<Schedulable>(*this);
	ar.serialize("count",    count,
	             "counting", counting);
}
INSTANTIATE_SERIALIZE_METHODS(EmuTimer);

} // namespace openmsx
