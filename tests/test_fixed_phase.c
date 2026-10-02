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

	for (unsigned int index = 0; index <= 256; ++index) {
		uint16_t atan_word = FIXED_PHASE_ATAN_Q15[index];
		uint16_t sine_word = FIXED_PHASE_QUARTER_Q15[index];

		atan_hash = hash_word(atan_hash, atan_word);
		sine_hash = hash_word(sine_hash, sine_word);
	}
	CHECK(atan_hash == UINT32_C(0x319ca829) &&
		      sine_hash == UINT32_C(0x22a0d741),
	      "generated fixed-phase profiles changed: %#x/%#x",
	      atan_hash, sine_hash);
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
	CHECK(hash == UINT32_C(0xa10ef8c5),
	      "fixed atan perimeter changed: %#x", hash);
	return true;
}

static bool check_atan_accuracy(int32_t q, int32_t i)
{
	uint16_t phase;
	double expected;
	double error;

	if (q == 0 && i == 0)
		return true;
	phase = fixed_phase_atan2_u15(q, i);
	expected = atan2((double)q, (double)i) * 32768.0 / (2.0 * PI);
	if (expected < 0)
		expected += 32768.0;
	error = fabs(phase - expected);
	if (error > 16384.0)
		error = 32768.0 - error;
	CHECK(phase < 0x8000 && error <= 22.0,
	      "atan(%d,%d) phase %u differs by %.9f units from %.9f",
	      q, i, phase, error, expected);
	return true;
}

static bool test_atan_angle_accuracy(void)
{
	CHECK(fixed_phase_atan2_u15(3, 3) == 0x1000 &&
	      fixed_phase_atan2_u15(-3, -3) == 0x5000,
	      "small equal components must yield diagonal phases");
	for (int q = -512; q <= 512; ++q)
		for (int i = -512; i <= 512; ++i)
			CHECK(check_atan_accuracy((int16_t)q, (int16_t)i),
			      "small-vector atan accuracy failed");
	for (uint32_t word = 0; word <= UINT16_MAX; ++word) {
		int16_t value = word_to_s16((uint16_t)word);
		int32_t wide = (int32_t)(INT64_C(65537) * word + INT32_MIN);

		CHECK(check_atan_accuracy(value, INT16_MAX) &&
		      check_atan_accuracy(value, INT16_MIN) &&
		      check_atan_accuracy(INT16_MAX, value) &&
		      check_atan_accuracy(INT16_MIN, value) &&
		      check_atan_accuracy(wide, INT32_MAX) &&
		      check_atan_accuracy(wide, INT32_MIN) &&
		      check_atan_accuracy(INT32_MAX, wide) &&
		      check_atan_accuracy(INT32_MIN, wide),
		      "atan accuracy failed on the signed perimeter");
	}
	return true;
}

static bool test_atan_scaling_invariance(void)
{
	for (int q = -16; q <= 16; ++q) {
		for (int i = -16; i <= 16; ++i) {
			uint16_t phase = fixed_phase_atan2_u15((int16_t)q, (int16_t)i);

			for (int scale = 2; scale <= 2047; ++scale) {
				uint16_t scaled = fixed_phase_atan2_u15(
					(int16_t)(q * scale), (int16_t)(i * scale));

				CHECK(scaled == phase,
				      "scaling (%d,%d) by %d changes phase %u to %u",
				      q, i, scale, phase, scaled);
			}
		}
	}
	for (int32_t ratio = 0; ratio <= 32768; ++ratio) {
		uint16_t phase = fixed_phase_atan2_u15(ratio, 32768);

		CHECK(check_atan_accuracy(ratio, 32768) &&
		      fixed_phase_atan2_u15(ratio * 65535, INT32_C(2147450880)) ==
			      phase,
		      "wide atan scaling changed at ratio %d", ratio);
	}
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
		{ "atan angle accuracy", test_atan_angle_accuracy },
		{ "atan scaling invariance", test_atan_scaling_invariance },
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
