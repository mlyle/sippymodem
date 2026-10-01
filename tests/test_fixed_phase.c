// SPDX-License-Identifier: BSD-3-Clause

#include <fixed_phase.h>
#include <phase_profile.h>

#include <limits.h>
#include <math.h>
#include <stdint.h>

#include "test.h"

static const double PI = 3.1415926535897932384626433832795;

static uint32_t hash_word(uint32_t hash, uint16_t word)
{
	return hash * UINT32_C(65599) + word;
}

static uint64_t hash_word64(uint64_t hash, uint16_t word)
{
	for (unsigned int byte = 0; byte < 2; ++byte) {
		hash ^= (word >> (byte * 8u)) & 0xffu;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static int16_t word_to_s16(uint16_t word)
{
	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
}

static bool test_generated_endpoints(void)
{
	uint32_t atan_hash = UINT32_C(2166136261);
	uint32_t sine_hash = UINT32_C(2166136261);
	uint32_t cosine_hash = UINT32_C(2166136261);

	for (unsigned int index = 0; index <= 256; ++index) {
		uint16_t atan_word = (uint16_t)lround(
			atan((double)index / 256.0) * 32768.0);
		uint16_t sine_word = FIXED_PHASE_QUARTER_Q15[index];
		uint16_t cosine_word = (uint16_t)(uint32_t)(
			cos(PI * (double)index / 512.0) * 32768.0);

		atan_hash = hash_word(atan_hash, atan_word);
		sine_hash = hash_word(sine_hash, sine_word);
		cosine_hash = hash_word(cosine_hash, cosine_word);
	}
	CHECK(atan_hash == UINT32_C(0x319ca829) &&
		      sine_hash == UINT32_C(0x22a0d741) &&
		      cosine_hash == UINT32_C(0x014b0f41),
	      "generated fixed-phase profiles changed: %#x/%#x/%#x",
	      atan_hash, sine_hash, cosine_hash);
	return true;
}

static bool test_positive_phasor_profile(void)
{
	uint32_t hash = UINT32_C(2166136261);

	/* Keep the complete positive profile bit-exact for existing consumers. */
	for (uint32_t word = 0; word <= INT16_MAX; ++word) {
		struct fixed_phase_q14 sample =
			fixed_phase_sin_cos_q14((int16_t)word);

		hash = hash_word(hash, (uint16_t)sample.sine);
		hash = hash_word(hash, (uint16_t)sample.cosine);
	}
	CHECK(hash == UINT32_C(0xb9bd06c5),
	      "positive fixed phasor profile changed: %#x", hash);
	CHECK(fixed_phase_sin_cos_q14(0).sine == 0 &&
		      fixed_phase_sin_cos_q14(0).cosine == 16384 &&
		      fixed_phase_sin_cos_q14(0x2000).sine == 16384 &&
		      fixed_phase_sin_cos_q14(0x2000).cosine == 0 &&
		      fixed_phase_sin_cos_q14(0x4000).sine == 0 &&
		      fixed_phase_sin_cos_q14(0x4000).cosine == -16384 &&
		      fixed_phase_sin_cos_q14(0x6000).sine == -16384 &&
		      fixed_phase_sin_cos_q14(0x6000).cosine == 0 &&
		      fixed_phase_sin_cos_q14((int16_t)-24576).sine == 16384 &&
		      fixed_phase_sin_cos_q14((int16_t)-24576).cosine == 0,
	      "fixed phasor cardinal or signed-phase behavior changed");
	return true;
}

static bool test_signed_phase_wrap_and_bounds(void)
{
	for (int32_t phase = 0; phase <= INT16_MAX; ++phase) {
		struct fixed_phase_q14 positive =
			fixed_phase_sin_cos_q14((int16_t)phase);
		struct fixed_phase_q14 negative =
			fixed_phase_sin_cos_q14((int16_t)(phase - 32768));
		double radians = 2.0 * PI * phase / 32768.0;

		CHECK(positive.sine == negative.sine &&
			      positive.cosine == negative.cosine,
		      "phase aliases differ at %d: %d/%d versus %d/%d",
		      (int)phase, positive.sine, positive.cosine,
		      negative.sine, negative.cosine);
		CHECK(positive.sine >= -16384 && positive.sine <= 16384 &&
			      positive.cosine >= -16384 &&
			      positive.cosine <= 16384,
		      "phasor exceeds unit Q14 amplitude at %d: %d/%d",
		      (int)phase, positive.sine, positive.cosine);
		CHECK(fabs(positive.sine - sin(radians) * 16384.0) <= 2.0 &&
			      fabs(positive.cosine - cos(radians) * 16384.0) <= 2.0,
		      "phasor interpolation exceeds two Q14 units at %d",
		      (int)phase);
	}
	return true;
}

static bool test_complete_unsigned_carrier_profile(void)
{
	uint64_t hash = UINT64_C(1469598103934665603);

	for (uint32_t word = 0; word <= UINT16_MAX; ++word) {
		struct fixed_phase_q14 sample =
			fixed_phase_sin_cos_u16_q14((uint16_t)word);

		hash = hash_word64(hash, (uint16_t)sample.sine);
		hash = hash_word64(hash, (uint16_t)sample.cosine);
	}
	CHECK(hash == UINT64_C(0x777ab1af713b75bb),
	      "complete unsigned carrier profile changed: %#llx",
	      (unsigned long long)hash);
	CHECK(fixed_phase_sin_cos_u16_q14(0).sine == 0 &&
		      fixed_phase_sin_cos_u16_q14(0).cosine == 16384 &&
		      fixed_phase_sin_cos_u16_q14(1).sine == 2 &&
		      fixed_phase_sin_cos_u16_q14(0x4000).sine == 16384 &&
		      fixed_phase_sin_cos_u16_q14(0x4000).cosine == 0 &&
		      fixed_phase_sin_cos_u16_q14(0x8000).cosine == -16384 &&
		      fixed_phase_sin_cos_u16_q14(UINT16_MAX).sine == -2,
	      "unsigned carrier cardinal or interpolation behavior changed");
	return true;
}

static bool test_compact_cosine_profile(void)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (unsigned int phase = 0; phase < 256; ++phase) {
		int16_t cosine = fixed_phase_cosine_u8_q14((uint8_t)phase);

		hash = hash_word64(hash, (uint16_t)cosine);
	}
	CHECK(hash == UINT64_C(0x34d86b711379ea60) &&
		      fixed_phase_cosine_u8_q14(0) == 16384 &&
		      fixed_phase_cosine_u8_q14(64) == 0 &&
		      fixed_phase_cosine_u8_q14(128) == -16383 &&
		      fixed_phase_cosine_u8_q14(192) == 0,
	      "compact cosine profile changed: %#llx",
	      (unsigned long long)hash);
	return true;
}

static bool test_atan_axes_and_perimeter(void)
{
	uint32_t hash = UINT32_C(2166136261);

	CHECK(fixed_phase_atan2_u15(0, 1) == 0 &&
		      fixed_phase_atan2_u15(1, 0) == 0x2000 &&
		      fixed_phase_atan2_u15(0, -1) == 0x4000 &&
		      fixed_phase_atan2_u15(-1, 0) == 0x6000 &&
		      fixed_phase_atan2_u15(0, 0) == 0x2000 &&
		      fixed_phase_atan2_u15(1, 1) == 0x1000,
	      "fixed atan axis convention changed");
	for (uint32_t word = 0; word <= UINT16_MAX; ++word) {
		int16_t value = word_to_s16((uint16_t)word);

		hash = hash_word(
			hash, fixed_phase_atan2_u15(value, INT16_MAX));
		hash = hash_word(
			hash, fixed_phase_atan2_u15(value, INT16_MIN));
		hash = hash_word(
			hash, fixed_phase_atan2_u15(INT16_MAX, value));
		hash = hash_word(
			hash, fixed_phase_atan2_u15(INT16_MIN, value));
	}
	CHECK(hash == UINT32_C(0xc3be1f43),
	      "fixed atan perimeter changed: %#x", hash);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated endpoints", test_generated_endpoints },
		{ "positive phasor profile", test_positive_phasor_profile },
		{ "signed phase wrap and bounds", test_signed_phase_wrap_and_bounds },
		{ "complete unsigned carrier profile",
		  test_complete_unsigned_carrier_profile },
		{ "compact cosine profile", test_compact_cosine_profile },
		{ "atan axes and perimeter", test_atan_axes_and_perimeter },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
