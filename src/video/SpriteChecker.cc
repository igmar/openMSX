/*
TODO:
- Verify model for 5th sprite number calculation.
  For example, does it have the right value in text mode?
- Further investigate sprite collision registers:
   - If there is NO collision, the value of these registers constantly changes.
     Could this be some kind of indication for the scanline XY coords???
   - Bit 9 of the Y coord (odd/even page??) is not yet implemented.
*/

#include "SpriteChecker.hh"

#include "RenderSettings.hh"

#include "BooleanSetting.hh"
#include "serialize.hh"

#include <algorithm>
#include <bit>
#include <cassert>

namespace openmsx {

SpriteChecker::SpriteChecker(VDP& vdp_, RenderSettings& renderSettings,
                             EmuTime time)
	: vdp(vdp_), vram(vdp.getVRAM())
	, limitSpritesSetting(renderSettings.getLimitSpritesSetting())
	, frameStartTime(time)
{
	vram.spriteAttribTable.setObserver(this);
	vram.spritePatternTable.setObserver(this);
}

void SpriteChecker::reset(EmuTime time)
{
	vdp.setSpriteStatus(0); // TODO 0x00 or 0x1F  (blueMSX has 0x1F)
	collisionX = 0;
	collisionY = 0;
	collisionHeld = false;
	releaseArmed = false;
	releaseNextFrame = false;

	frameStart(time);

	updateSpritesMethod = &SpriteChecker::updateSprites1;
}

inline SpriteChecker::SpritePattern SpriteChecker::calculatePatternNP(
	unsigned patternNr, unsigned y) const
{
	auto patternPtr = vram.spritePatternTable.getReadArea<256 * 8>(0);
	unsigned index = patternNr * 8 + y;
	SpritePattern pattern = patternPtr[index] << 24;
	if (vdp.getSpriteSize() == 16) {
		pattern |= patternPtr[index + 16] << 16;
	}
	return !vdp.isSpriteMag() ? pattern : doublePattern(pattern);
}
inline SpriteChecker::SpritePattern SpriteChecker::calculatePatternPlanar(
	unsigned patternNr, unsigned y) const
{
	auto [ptr0, ptr1] = vram.spritePatternTable.getReadAreaPlanar<256 * 8>(0);
	unsigned index = patternNr * 8 + y;
	auto patternPtr = (index & 1) ? ptr1 : ptr0;
	index /= 2;
	SpritePattern pattern = patternPtr[index] << 24;
	if (vdp.getSpriteSize() == 16) {
		pattern |= patternPtr[index + (16 / 2)] << 16;
	}
	return !vdp.isSpriteMag() ? pattern : doublePattern(pattern);
}

void SpriteChecker::updateSprites1(int limit)
{
	if (vdp.spritesEnabledFast()) {
		if (vdp.isDisplayEnabled()) {
			// in display area
			checkSprites1(currentLine, limit);
		} else {
			// in border, only check last line of top border
			int l0 = vdp.getLineZero() - 1;
			if ((currentLine <= l0) && (l0 < limit)) {
				checkSprites1(l0, l0 + 1);
			}
		}
	}
	currentLine = limit;
}

inline SpriteChecker::FillResult SpriteChecker::fillSprites1(int minLine, int maxLine)
{
	// This implementation contains a double for-loop. The outer loop goes
	// over the sprites, the inner loop over the to-be-checked lines. This
	// is not the order in which the real VDP performs this operation: the
	// real VDP renders line-per-line and for each line checks all 32
	// sprites.
	//
	// Though this 'reverse' order allows to skip over very large regions
	// of the inner loop: we only have to process the lines were a
	// particular sprite is actually visible. I measured this makes this
	// routine 4x-5x faster!
	//
	// This routine also needs to detect the sprite number of the 'first'
	// 5th-sprite-condition. With 'first' meaning the first line where this
	// condition occurs. Because our loops are swapped compared to the real
	// VDP, we need some extra fixup logic to correctly detect this.

	// Calculate display line.
	// This is the line sprites are checked at; the line they are displayed
	// at is one lower.
	int displayDelta = vdp.getVerticalScroll() - vdp.getLineZero();

	// Get sprites for this line and detect 5th sprite if any.
	bool limitSprites = limitSpritesSetting.getBoolean();
	int size = vdp.getSpriteSize();
	bool mag = vdp.isSpriteMag();
	int magSize = (mag + 1) * size;
	auto attributePtr = vram.spriteAttribTable.getReadArea<32 * 4>(0);
	uint8_t patternIndexMask = size == 16 ? 0xFC : 0xFF;
	int fifthSpriteNum  = -1;  // no 5th sprite detected yet
	int fifthSpriteLine = 999; // larger than any possible valid line

	int sprite = 0;
	for (/**/; sprite < 32; ++sprite) {
		int y = attributePtr[4 * sprite + 0];
		if (y == 208) break;

		for (int line = minLine; line < maxLine; ++line) { // 'line' changes in loop
			// Calculate line number within the sprite.
			int displayLine = line + displayDelta;
			int spriteLine = (displayLine - y) & 0xFF;
			if (spriteLine >= magSize) {
				// Skip ahead till sprite becomes visible.
				line += 256 - spriteLine - 1; // -1 because of for-loop
				continue;
			}

			auto visibleIndex = spriteCount[line];
			if (visibleIndex == 4) {
				// Find earliest line where this condition occurs.
				if (line < fifthSpriteLine && line != earlySpriteLine) {
					fifthSpriteLine = line;
					fifthSpriteNum = sprite;
				}
				if (limitSprites) continue;
			}

			SpriteInfo& sip = spriteBuffer[line][visibleIndex];
			int patternIndex = attributePtr[4 * sprite + 2] & patternIndexMask;
			if (mag) spriteLine /= 2;
			sip.pattern = calculatePatternNP(patternIndex, spriteLine);
			sip.x = attributePtr[4 * sprite + 1];
			uint8_t colorAttrib = attributePtr[4 * sprite + 3];
			if (colorAttrib & 0x80) sip.x -= 32;
			sip.colorAttrib = colorAttrib;

			spriteCount[line] = visibleIndex + 1;
		}
	}
	return {.nthSprite = fifthSpriteNum, .numSprites = sprite};
}

inline void SpriteChecker::checkSprites1(int minLine, int maxLine)
{
	auto [fifthSpriteNum, sprite] = fillSprites1(minLine, maxLine);

	// Update status register.
	uint8_t status = vdp.getStatusReg0();
	if (fifthSpriteNum != -1) {
		// Five sprites on a line.
		// According to TMS9918.pdf 5th sprite detection is only
		// active when F flag is zero.
		if ((status & 0xC0) == 0) {
			status = uint8_t(0x40 | (status & 0x20) | fifthSpriteNum);
		}
	}
	if (~status & 0x40) {
		// No 5th sprite detected, store number of latest sprite processed.
		status = (status & 0x20) | uint8_t(std::min(sprite, 31));
	}
	vdp.setSpriteStatus(status);

	// Optimisation:
	// If collision already occurred,
	// that state is stable until it is reset by a status reg read,
	// so no need to execute the checks.
	// The spriteBuffer array is filled now, so we can bail out.
	if ((vdp.getStatusReg0() & 0x20) && !wantCollision()) return;

	/*
	Model for sprite collision: (or "coincidence" in TMS9918 data sheet)
	- Reset when status reg is read.
	- Set when sprite patterns overlap.
	- ??? Color doesn't matter: sprites of color 0 can collide.
	  ??? This conflicts with: https://github.com/openMSX/openMSX/issues/1198
	- Sprites that are partially off-screen position can collide, but only
	  on the in-screen pixels. In other words: sprites cannot collide in
	  the left or right border, only in the visible screen area. Though
	  they can collide in the V9958 extra border mask. This behaviour is
	  the same in sprite mode 1 and 2.

	Implemented by checking every pair for collisions.
	For large numbers of sprites that would be slow,
	but there are max 4 sprites and therefore max 6 pairs.
	If any collision is found, method returns at once.
	*/
	// verified: collision coords are also filled in for sprite mode 1
	if (pendingCollisionLine != -1) return; // not flagged yet
	for (auto line : xrange(minLine, maxLine)) {
		if (auto minXCollision = findCollision1(line)) {
			foundCollision(line, *minXCollision);
			return; // don't check lines with higher Y-coord
		}
	}
}

std::optional<int> SpriteChecker::findCollision1(int line) const
{
	bool can0collide = vdp.canSpriteColor0Collide();
	int magSize = (vdp.isSpriteMag() + 1) * vdp.getSpriteSize();
	int minXCollision = 999; // no collision
	for (int i = std::min<int>(4, spriteCount[line]); --i >= 1; /**/) {
		auto color1 = spriteBuffer[line][i].colorAttrib & 0xf;
		if (!can0collide && (color1 == 0)) continue;
		int x_i = spriteBuffer[line][i].x;
		SpritePattern pattern_i = spriteBuffer[line][i].pattern;
		for (int j = i; --j >= 0; /**/) {
			auto color2 = spriteBuffer[line][j].colorAttrib & 0xf;
			if (!can0collide && (color2 == 0)) continue;
			// Do sprite i and sprite j collide?
			int x_j = spriteBuffer[line][j].x;
			int dist = x_j - x_i;
			if ((-magSize < dist) && (dist < magSize)) {
				SpritePattern pattern_j = spriteBuffer[line][j].pattern;
				if (dist < 0) {
					pattern_j <<= -dist;
				} else {
					pattern_j >>= dist;
				}
				SpritePattern colPat = pattern_i & pattern_j;
				if (x_i < 0) {
					assert(x_i >= -32);
					colPat &= (1 << (32 + x_i)) - 1;
				}
				if (colPat) {
					int xCollision = x_i + std::countl_zero(colPat);
					assert(xCollision >= 0);
					minXCollision = std::min(minXCollision, xCollision);
				}
			}
		}
	}
	if (minXCollision >= 256) return std::nullopt;
	return minXCollision;
}

void SpriteChecker::updateSprites2(int limit)
{
	// TODO merge this with updateSprites1()?
	if (vdp.spritesEnabledFast()) {
		if (vdp.isDisplayEnabled()) {
			// in display area
			checkSprites2(currentLine, limit);
		} else {
			// in border, only check last line of top border
			int l0 = vdp.getLineZero() - 1;
			if ((currentLine <= l0) && (l0 < limit)) {
				checkSprites2(l0, l0 + 1);
			}
		}
	}
	currentLine = limit;
}

inline SpriteChecker::FillResult SpriteChecker::fillSprites2(int minLine, int maxLine)
{
	// See comment in fillSprites1() about order of inner and outer loops.

	// Calculate display line.
	// This is the line sprites are checked at; the line they are displayed
	// at is one lower.
	int displayDelta = vdp.getVerticalScroll() - vdp.getLineZero();

	// Get sprites for this line and detect 5th sprite if any.
	bool limitSprites = limitSpritesSetting.getBoolean();
	int size = vdp.getSpriteSize();
	bool mag = vdp.isSpriteMag();
	int magSize = (mag + 1) * size;
	int patternIndexMask = (size == 16) ? 0xFC : 0xFF;
	int ninthSpriteNum  = -1;  // no 9th sprite detected yet
	int ninthSpriteLine = 999; // larger than any possible valid line

	// Because it gave a measurable performance boost, we duplicated the
	// code for planar and non-planar modes.
	int sprite = 0;
	if (planar) {
		auto [attributePtr0, attributePtr1] =
			vram.spriteAttribTable.getReadAreaPlanar<32 * 4>(512);
		// TODO: Verify CC implementation.
		for (/**/; sprite < 32; ++sprite) {
			int y = attributePtr0[2 * sprite + 0];
			if (y == 216) break;

			for (int line = minLine; line < maxLine; ++line) { // 'line' changes in loop
				// Calculate line number within the sprite.
				int displayLine = line + displayDelta;
				int spriteLine = (displayLine - y) & 0xFF;
				if (spriteLine >= magSize) {
					// Skip ahead till sprite is visible.
					line += 256 - spriteLine - 1;
					continue;
				}

				auto visibleIndex = spriteCount[line];
				if (visibleIndex == 8) {
					// Find earliest line where this condition occurs.
					if (line < ninthSpriteLine && line != earlySpriteLine) {
						ninthSpriteLine = line;
						ninthSpriteNum = sprite;
					}
					if (limitSprites) continue;
				}

				if (mag) spriteLine /= 2;
				unsigned colorIndex = (~0u << 10) | (sprite * 16 + spriteLine);
				uint8_t colorAttrib =
					vram.spriteAttribTable.readPlanar(colorIndex);

				SpriteInfo& sip = spriteBuffer[line][visibleIndex];
				int patternIndex = attributePtr0[2 * sprite + 1] & patternIndexMask;
				sip.pattern = calculatePatternPlanar(patternIndex, spriteLine);
				sip.x = attributePtr1[2 * sprite + 0];
				if (colorAttrib & 0x80) sip.x -= 32;
				sip.colorAttrib = colorAttrib;

				// set sentinel (see below)
				spriteBuffer[line][visibleIndex + 1].colorAttrib = 0;
				spriteCount[line] = visibleIndex + 1;
			}
		}
	} else {
		auto attributePtr0 =
			vram.spriteAttribTable.getReadArea<32 * 4>(512);
		// TODO: Verify CC implementation.
		for (/**/; sprite < 32; ++sprite) {
			int y = attributePtr0[4 * sprite + 0];
			if (y == 216) break;

			for (int line = minLine; line < maxLine; ++line) { // 'line' changes in loop
				// Calculate line number within the sprite.
				int displayLine = line + displayDelta;
				int spriteLine = (displayLine - y) & 0xFF;
				if (spriteLine >= magSize) {
					// Skip ahead till sprite is visible.
					line += 256 - spriteLine - 1;
					continue;
				}

				auto visibleIndex = spriteCount[line];
				if (visibleIndex == 8) {
					// Find earliest line where this condition occurs.
					if (line < ninthSpriteLine && line != earlySpriteLine) {
						ninthSpriteLine = line;
						ninthSpriteNum = sprite;
					}
					if (limitSprites) continue;
				}

				if (mag) spriteLine /= 2;
				unsigned colorIndex = (~0u << 10) | (sprite * 16 + spriteLine);
				uint8_t colorAttrib =
					vram.spriteAttribTable.readNP(colorIndex);
				// Sprites with CC=1 are only visible if preceded by
				// a sprite with CC=0. However they DO contribute towards
				// the max-8-sprites-per-line limit, so we can't easily
				// filter them here. See also
				//    https://github.com/openMSX/openMSX/issues/497

				SpriteInfo& sip = spriteBuffer[line][visibleIndex];
				int patternIndex = attributePtr0[4 * sprite + 2] & patternIndexMask;
				sip.pattern = calculatePatternNP(patternIndex, spriteLine);
				sip.x = attributePtr0[4 * sprite + 1];
				if (colorAttrib & 0x80) sip.x -= 32;
				sip.colorAttrib = colorAttrib;

				// Set sentinel. Sentinel is actually only
				// needed for sprites with CC=1.
				// In the past we set the sentinel (for all
				// lines) at the end. But it's slightly faster
				// to do it only for lines that actually
				// contain sprites (even if sentinel gets
				// overwritten a couple of times for lines with
				// many sprites).
				spriteBuffer[line][visibleIndex + 1].colorAttrib = 0;
				spriteCount[line] = visibleIndex + 1;
			}
		}
	}
	return {.nthSprite = ninthSpriteNum, .numSprites = sprite};
}

inline void SpriteChecker::checkSprites2(int minLine, int maxLine)
{
	auto [ninthSpriteNum, sprite] = fillSprites2(minLine, maxLine);

	// Update status register.
	uint8_t status = vdp.getStatusReg0();
	if (ninthSpriteNum != -1) {
		// Nine sprites on a line.
		// According to TMS9918.pdf 5th sprite detection is only
		// active when F flag is zero. Stuck to this for V9938.
		// Dragon Quest 2 needs this.
		if ((status & 0xC0) == 0) {
			status = uint8_t(0x40 | (status & 0x20) | ninthSpriteNum);
		}
	}
	if (~status & 0x40) {
		// No 9th sprite detected, store number of latest sprite processed.
		status = (status & 0x20) | uint8_t(std::min(sprite, 31));
	}
	vdp.setSpriteStatus(status);

	// Optimisation:
	// If collision already occurred,
	// that state is stable until it is reset by a status reg read,
	// so no need to execute the checks.
	// The visibleSprites array is filled now, so we can bail out.
	if ((vdp.getStatusReg0() & 0x20) && !wantCollision()) return;

	/*
	Model for sprite collision: (or "coincidence" in TMS9918 data sheet)
	- Reset when status reg is read.
	- Set when sprite patterns overlap.
	- ??? Color doesn't matter: sprites of color 0 can collide.
	  ???  TODO: V9938 data book denies this (page 98).
	  ??? This conflicts with: https://github.com/openMSX/openMSX/issues/1198
	- Sprites that are partially off-screen position can collide, but only
	  on the in-screen pixels. In other words: sprites cannot collide in
	  the left or right border, only in the visible screen area. Though
	  they can collide in the V9958 extra border mask. This behaviour is
	  the same in sprite mode 1 and 2.

	Implemented by checking every pair for collisions.
	For large numbers of sprites that would be slow.
	There are max 8 sprites and therefore max 42 pairs.
	  TODO: Maybe this is slow... Think of something faster.
	        Probably new approach is needed anyway for OR-ing.
	*/
	if (pendingCollisionLine != -1) return; // not flagged yet
	for (auto line : xrange(minLine, maxLine)) {
		if (auto minXCollision = findCollision2(line)) {
			foundCollision(line, *minXCollision);
			return; // don't check lines with higher Y-coord
		}
	}
}

std::optional<int> SpriteChecker::findCollision2(int line) const
{
	bool can0collide = vdp.canSpriteColor0Collide();
	int magSize = (vdp.isSpriteMag() + 1) * vdp.getSpriteSize();
	int minXCollision = 999; // no collision
	std::span<const SpriteInfo, 32 + 1> visibleSprites = spriteBuffer[line];
	for (int i = std::min<int>(8, spriteCount[line]); --i >= 1; /**/) {
		auto colorAttrib1 = visibleSprites[i].colorAttrib;
		if (!can0collide && ((colorAttrib1 & 0xf) == 0)) continue;
		// If CC or IC is set, this sprite cannot collide.
		if (colorAttrib1 & 0x60) continue;

		int x_i = visibleSprites[i].x;
		SpritePattern pattern_i = visibleSprites[i].pattern;
		for (int j = i; --j >= 0; /**/) {
			auto colorAttrib2 = visibleSprites[j].colorAttrib;
			if (!can0collide && ((colorAttrib2 & 0xf) == 0)) continue;
			// If CC or IC is set, this sprite cannot collide.
			if (colorAttrib2 & 0x60) continue;

			// Do sprite i and sprite j collide?
			int x_j = visibleSprites[j].x;
			int dist = x_j - x_i;
			if ((-magSize < dist) && (dist < magSize)) {
				SpritePattern pattern_j = visibleSprites[j].pattern;
				if (dist < 0) {
					pattern_j <<= -dist;
				} else {
					pattern_j >>= dist;
				}
				SpritePattern colPat = pattern_i & pattern_j;
				if (x_i < 0) {
					assert(x_i >= -32);
					colPat &= (1 << (32 + x_i)) - 1;
				}
				if (colPat) {
					int xCollision = x_i + std::countl_zero(colPat);
					assert(xCollision >= 0);
					minXCollision = std::min(minXCollision, xCollision);
				}
			}
		}
	}
	if (minXCollision >= 256) return std::nullopt;
	return minXCollision;
}

// A collision found in check line n is flagged while line n + 1, where those
// sprites are displayed, is drawn: when the beam is about 5 pixels past the
// colliding pixel. The coordinates are then x + 12 and that display line + 8.
// Measured on a Philips VG-8235 (V9958) with a bench test ROM: two sprites
// overlapping at X 128, shown from display line 101, set C 840 - 1008 VDP
// cycles after FH of line 100 (so in line 101, near dot 125 - 165), and S#5
// reads 109. (The 5-pixel delay was measured before, on a V9958, with the
// collision loop of the MSX2+ boot logo.)
// Not measured on the TMS99xx, so there the collision is still flagged at the
// end of the check line, with the check line's coordinates.
void SpriteChecker::foundCollision(int line, int minXCollision)
{
	if (vdp.isMSX1VDP()) {
		vdp.setSpriteStatus(vdp.getStatusReg0() | 0x20);
		collisionX = minXCollision + 12;
		collisionY = line - vdp.getLineZero() + 8;
		return;
	}
	pendingCollisionLine = line;
	pendingCollisionX = minXCollision;
}

void SpriteChecker::flagPendingCollision(EmuTime time)
{
	static constexpr int COLLISION_DELAY = 5; // pixels
	int displayLine = pendingCollisionLine + 1;
	int flagTicks = displayLine * VDP::TICKS_PER_LINE + vdp.getLeftSprites()
	              + 4 * (pendingCollisionX + COLLISION_DELAY);
	if (frameStartTime.getTicksTill(time) < unsigned(flagTicks)) return;
	vdp.setSpriteStatus(vdp.getStatusReg0() | 0x20);
	updateHold(flagTicks);
	if (!collisionHeld) {
		collisionX = pendingCollisionX + 12;
		collisionY = displayLine - vdp.getLineZero() + 8;
		collisionHeld = true;
	} else {
		// a collision while a pair is held disarms the release
		releaseArmed = false;
	}
	pendingCollisionLine = -1;
}

// On a V99x8, S#3 - S#6 hold the coordinates of a collision until a release
// that an S#5 read arms; without a held collision they show the beam
// position. Measured on a Philips VG-8235 (V9958), GRAPHIC4, sprite mode 2,
// with a bench test ROM, seven runs over 10 days:
// - A collision latches X and Y. An S#0 read does not release them, an S#5
//   read arms the release (S#0's presence or order makes no difference).
// - The release happens at a fixed point, not at the read: the held values
//   show up to line 304 and the beam from line 305 (display lines, 50 Hz;
//   254 at 60 Hz), here taken as the moment Y changes in that line. A read
//   after that point arms the release of the next frame.
// - One new pair is taken per release. A collision while a pair is held
//   disarms the read.
// - The S#5 read itself changes nothing that can be read.
// The V9938 was measured only in part; the TMS99xx has no S#3 - S#6.

// Frame line in which Y is reset (Y reads 0 in this line and the next);
// the release happens at Y's change in the next line.
static constexpr int BEAM_Y_RESET_PAL = 50;
static constexpr int BEAM_Y_RESET_NTSC = 23;
// Y changes 240 VDP ticks after HR rises, in graphic modes with R#18 = 0
// (R#18 doesn't move it): that's in the next line.
static constexpr int BEAM_Y_TICK = (100 + 102 + 56 + 1024 + 240) - VDP::TICKS_PER_LINE;

unsigned SpriteChecker::releaseTicks() const
{
	int line = (vdp.isPalTiming() ? BEAM_Y_RESET_PAL : BEAM_Y_RESET_NTSC) + 1;
	return unsigned(line * VDP::TICKS_PER_LINE + BEAM_Y_TICK);
}

void SpriteChecker::updateHold(uint64_t ticks)
{
	if (!collisionHeld || !releaseArmed || releaseNextFrame) return;
	if (ticks < releaseTicks()) return;
	collisionHeld = false;
	releaseArmed = false;
}

bool SpriteChecker::wantCollision() const
{
	// A new collision takes new coordinates, or disarms a release, even
	// when the collision flag is already set.
	return !vdp.isMSX1VDP() && (!collisionHeld || releaseArmed);
}

void SpriteChecker::resetCollision(EmuTime time)
{
	auto ticks = frameStartTime.getTicksTill(time);
	updateHold(ticks);
	if (!collisionHeld) return;
	releaseArmed = true;
	releaseNextFrame = ticks >= releaseTicks();
}

bool SpriteChecker::beamStopped() const
{
	// with sprites disabled (R#8 SPD) or in a mode without sprites the
	// beam counters read X 22 and Y 8 (measured, not understood)
	return !vdp.spritesEnabledRegister() ||
	       (vdp.getDisplayMode().getSpriteMode(vdp.isMSX1VDP()) == 0);
}

int SpriteChecker::getBeamX(EmuTime time) const
{
	// X = 2 * floor((h - 317) / 8), wrapped at 342, with h the VDP ticks
	// after HR rises (good to +-2 ticks on 317). Between two counts the
	// real VDP sometimes reads a neighbouring count or the AND of two.
	if (beamStopped()) return 22;
	int ticks = int(frameStartTime.getTicksTill(time) % VDP::TICKS_PER_LINE);
	int h = (ticks - vdp.getRightBorder() + 2 * VDP::TICKS_PER_LINE)
	      % VDP::TICKS_PER_LINE;
	int count = (h - 317 + 8 * VDP::TICKS_PER_LINE) / 8 - VDP::TICKS_PER_LINE;
	return ((2 * count) % 342 + 342) % 342;
}

int SpriteChecker::getBeamY(EmuTime time) const
{
	// Y counts the lines: it reads 0 in the reset line and the next, then
	// 1, 2, ... (so display line + 8, with R#18 = 0 and LN = 1), changing
	// 240 ticks after HR rises.
	auto ticks = frameStartTime.getTicksTill(time);
	int line = narrow<int>(ticks / VDP::TICKS_PER_LINE);
	if (int(ticks % VDP::TICKS_PER_LINE) < BEAM_Y_TICK) --line;
	int reset = vdp.isPalTiming() ? BEAM_Y_RESET_PAL : BEAM_Y_RESET_NTSC;
	int y = 0;
	if (line > reset + 1) {
		y = line - reset - 1;
	} else if (line < reset) {
		y = line + prevFrameLines - reset - 1;
	}
	// TODO: the real VDP stops Y where it is when sprites are disabled,
	// and counts from the reset up to 8; here it reads at most 8.
	if (beamStopped()) y = std::min(y, 8);
	return y & 0x1FF;
}

void SpriteChecker::checkStatusEarly(EmuTime time)
{
	// Not measured on the TMS99xx, so it keeps flagging at the end of the
	// line.
	if (vdp.isMSX1VDP()) return;
	bool mode1 = updateSpritesMethod == &SpriteChecker::updateSprites1;
	if (!mode1 && updateSpritesMethod != &SpriteChecker::updateSprites2) return;
	if (!vdp.spritesEnabledFast()) return;
	int ticks = narrow<int>(frameStartTime.getTicksTill(time));
	int line = ticks / VDP::TICKS_PER_LINE;
	if (line != currentLine) return;
	// as updateSprites1/2(): in the border only the last line of the top
	// border is checked (for the first display line)
	if (!vdp.isDisplayEnabled() && (line != vdp.getLineZero() - 1)) return;
	uint8_t status = vdp.getStatusReg0();
	// (The collision flag needs no early check: it goes up in the line
	// after the check line, see foundCollision().)
	bool wantSprite = !(status & 0xC0) && (line != earlySpriteLine);
	if (!wantSprite) return;

	int nthSprite = mode1 ? fillSprites1(line, line + 1).nthSprite
	                      : fillSprites2(line, line + 1).nthSprite;
	spriteCount[line] = 0; // updateSprites1/2() fill this line again
	int cycle = ticks % VDP::TICKS_PER_LINE;

	// The VDP reads the y-coordinates of the 32 sprites one by one during
	// the line, sprite n at cycle 182 + 32n in bitmap modes and at
	// 226 + 32n in character modes, each read taking 6 cycles (see
	// doc/internal/vdp-vram-timing). The 5th/9th sprite flag goes up once
	// it has read the sprite that doesn't fit.
	int firstRead = vdp.getDisplayMode().isBitmapMode() ? 182 : 226;
	if (wantSprite && (nthSprite != -1) &&
	    (cycle >= firstRead + 32 * nthSprite + 6)) {
		vdp.setSpriteStatus(uint8_t(0x40 | (vdp.getStatusReg0() & 0x20) | nthSprite));
		earlySpriteLine = line;
	}
}

// version 1: initial version
// version 2: bug fix: also serialize 'currentLine'
// version 3: added 'earlyCollisionLine' and 'earlySpriteLine'
// version 4: replaced 'earlyCollisionLine' by 'pendingCollisionLine' and
//            'pendingCollisionX'
// version 5: added 'collisionHeld', 'releaseArmed', 'releaseNextFrame' and
//            'prevFrameLines'
template<typename Archive>
void SpriteChecker::serialize(Archive& ar, unsigned version)
{
	if constexpr (Archive::IS_LOADER) {
		// Recalculate from VDP state:
		//  - frameStartTime
		frameStartTime.reset(vdp.getFrameStartTime());
		//  - updateSpritesMethod, planar
		setDisplayMode(vdp.getDisplayMode());

		// We don't serialize spriteCount[] and spriteBuffer[].
		// These are only used to draw the MSX screen, they don't have
		// any influence on the MSX state. So the effect of not
		// serializing these two is that no sprites will be shown in the
		// first (partial) frame after loadstate.
		std::ranges::fill(spriteCount, 0);
		// content of spriteBuffer[] doesn't matter if spriteCount[] is 0
	}
	ar.serialize("collisionX", collisionX,
	             "collisionY", collisionY);
	if (ar.versionAtLeast(version, 2)) {
		ar.serialize("currentLine", currentLine);
	} else {
		currentLine = 0;
	}
	if (ar.versionAtLeast(version, 4)) {
		ar.serialize("earlySpriteLine",      earlySpriteLine,
		             "pendingCollisionLine", pendingCollisionLine,
		             "pendingCollisionX",    pendingCollisionX);
	} else {
		if (ar.versionAtLeast(version, 3)) {
			int earlyCollisionLine = -1; // no longer used
			ar.serialize("earlyCollisionLine", earlyCollisionLine,
			             "earlySpriteLine",    earlySpriteLine);
		} else {
			// older versions never flagged early
			earlySpriteLine = -1;
		}
		// older versions flagged a collision at once
		pendingCollisionLine = -1;
		pendingCollisionX = 0;
	}
	if (ar.versionAtLeast(version, 5)) {
		ar.serialize("collisionHeld",    collisionHeld,
		             "releaseArmed",     releaseArmed,
		             "releaseNextFrame", releaseNextFrame,
		             "prevFrameLines",   prevFrameLines);
	} else {
		// older versions held the coordinates until an S#5 read
		collisionHeld = true;
		releaseArmed = false;
		releaseNextFrame = false;
		prevFrameLines = vdp.isPalTiming() ? 313 : 262;
	}
}
INSTANTIATE_SERIALIZE_METHODS(SpriteChecker);

} // namespace openmsx
