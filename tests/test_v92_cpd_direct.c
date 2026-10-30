// SPDX-License-Identifier: BSD-3-Clause

#include "wide_uint.h"

#include <v92_cpd_direct.h>
#include <v92_pcm_frame.h>
#include <integer_math.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	DIRECT_B1U_FRAMES = 48,
	DIRECT_B1U_SYMBOLS =
		DIRECT_B1U_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
};

static uint8_t expected_point_count(uint8_t rate)
{
	unsigned int bits = pcm_upstream_frame_bits(rate);
	uint8_t count = 0u;

	if (rate == 16u)
		return 27u;
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		unsigned int exponent = bits / V92_PCM_MAPPING_FRAME_SYMBOLS;
		uint8_t modulus;
		uint8_t required;

		if (symbol < bits % V92_PCM_MAPPING_FRAME_SYMBOLS)
			exponent++;
		modulus = (uint8_t)(1u << exponent);
		required = symbol % 4u == 3u ? modulus :
			(uint8_t)((modulus + 1u) / 2u);
		if (count < required)
			count = required;
	}
	return rate == 13u ? V92_CPD_DIRECT_MAX_POINTS : count;
}

static bool direct_sections_are_correct(
	const struct v92_cpd_parameters *cpd, enum g711_law law,
	uint8_t rate)
{
	uint8_t point_count = expected_point_count(rate);
	struct wide_uint product = wide_from_u64(1);
	struct wide_uint expected = wide_shift_left(wide_from_u64(1), pcm_upstream_frame_bits(rate));

	CHECK(cpd->modulus_present && cpd->filters_present &&
	      cpd->constellations_present &&
	      cpd->filter[V92_CPD_PRECODER_FEED_FORWARD].count == 0u &&
	      cpd->filter[V92_CPD_PRECODER_FEEDBACK].count == 0u &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD].count ==
		      V92_CPD_DIRECT_FILTER_TAPS &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		      .coefficient[0] == INT16_MAX &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		      .coefficient[1] == 0 &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		      .coefficient[2] == 0 &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		      .coefficient[3] == 0 &&
	      cpd->filter[V92_CPD_PREFILTER_FEEDBACK].count == 0u,
	      "rate %u law %u did not use the four-tap identity prefilter",
	      rate, law);
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		unsigned int exponent =
			pcm_upstream_frame_bits(rate) /
			V92_PCM_MAPPING_FRAME_SYMBOLS;

		if (symbol < pcm_upstream_frame_bits(rate) %
				     V92_PCM_MAPPING_FRAME_SYMBOLS)
			exponent++;
		unsigned int modulus = rate == 16u ?
			(symbol % 4u == 3u ? 27u : 54u) : (1u << exponent);

		CHECK(cpd->modulus[symbol] == modulus,
		      "rate %u law %u symbol %zu modulus changed",
		      rate, law, symbol);
		wide_multiply_u32(product, cpd->modulus[symbol], &product);
	}
	CHECK(rate == 16u ? wide_compare(product, expected) >= 0 : wide_compare(product, expected) == 0,
	      "rate %u law %u modulus product changed", rate, law);
	CHECK(cpd->constellation[0].count == point_count,
	      "rate %u law %u generated %u rather than %u points",
	      rate, law, cpd->constellation[0].count, point_count);
	for (uint8_t point = 0u; point < point_count; ++point) {
		int16_t expected_level;

		if (point_count <= V92_CPD_DIRECT_LINEAR_POINT_LIMIT) {
			expected_level = (int16_t)((2u * point + 1u) * 256u);
		} else {
			CHECK(g711_ucode_sample(
				      law, (uint8_t)(point + 1u), true,
				      &expected_level),
			      "rate %u law %u point %u could not be generated",
			      rate, law, point);
		}
		CHECK(cpd->constellation[0].positive_linear[point] ==
			      expected_level,
		      "rate %u law %u point %u changed", rate, law, point);
	}
	for (size_t phase = 0u; phase < V92_CPD_FRAME_INTERVALS; ++phase)
		CHECK(cpd->frame_constellation[phase] == 0u,
		      "rate %u law %u phase %zu changed constellation",
		      rate, law, phase);
	for (size_t index = 1u;
	     index < V92_CPD_MAX_CONSTELLATIONS; ++index)
		CHECK(cpd->constellation[index].count == 0u,
		      "rate %u law %u populated constellation %zu",
		      rate, law, index);
	return true;
}

static struct wide_uint distance(struct wide_uint left, struct wide_uint right)
{
	return wide_compare(left, right) >= 0 ? wide_subtract(left, right) : wide_subtract(right, left);
}

static bool gain_is_nearest(
	const struct v92_cpd_parameters *cpd, enum g711_law law)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	uint64_t energy = 0u;
	uint32_t gain = cpd->four_times_gain_q0_16;
	struct wide_uint target = wide_shift_left(wide_from_u64(DIRECT_B1U_SYMBOLS), 36);
	struct wide_uint selected;

	CHECK(v92_pcm_mapping_init(&mapping, law, cpd) ==
		      V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) ==
		      V92_PCM_FRAME_OK,
	      "direct mapping did not initialize for gain verification");
	memset(ones, 1, mapping.data_bits_per_frame);
	for (unsigned int index = 0u;
	     index < DIRECT_B1U_FRAMES; ++index) {
		struct v92_pcm_frame frame;

		CHECK(v92_pcm_frame_encode(
			      &transmitter, ones,
			      mapping.data_bits_per_frame,
			      &frame) == V92_PCM_FRAME_OK,
		      "B1u frame %u did not encode", index);
		for (size_t symbol = 0u;
		     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			uint64_t magnitude = frame.v[symbol] < 0 ?
				(uint64_t)(-(int64_t)frame.v[symbol]) :
				(uint64_t)frame.v[symbol];

			CHECK(magnitude == 0u ||
			      magnitude <= UINT64_MAX / magnitude,
			      "B1u energy overflowed");
			CHECK(energy <= UINT64_MAX - magnitude * magnitude,
			      "B1u accumulated energy overflowed");
			energy += magnitude * magnitude;
		}
	}
	selected = wide_product_u64((uint64_t)gain * gain, energy);
	if (gain > 1u)
		CHECK(wide_compare(distance(selected, target),
			distance(wide_product_u64((uint64_t)(gain - 1u) * (gain - 1u), energy), target)) <= 0,
		      "gain %u was farther than its lower neighbor", gain);
	if (gain < UINT16_MAX)
		CHECK(wide_compare(distance(selected, target),
			distance(wide_product_u64((uint64_t)(gain + 1u) * (gain + 1u), energy), target)) <= 0,
		      "gain %u was farther than its upper neighbor", gain);
	return true;
}

static bool test_rate_and_law_matrix(void)
{
	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u;
		     rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			struct v92_cpd_parameters cpd;
			uint8_t frame[V92_CPD_DIRECT_MAX_FRAME_BYTES];

			CHECK(v92_cpd_direct_init(
				      &cpd, (enum g711_law)law, rate,
				      (rate & 1u) != 0u) == V92_CPD_OK &&
			      v92_cpd_parameters_are_valid(&cpd),
			      "rate %u law %u did not generate", rate, law);
			struct v92_cpd_parameters geometry;

			CHECK(v92_cpd_direct_geometry(&geometry, (enum g711_law)law,
				      rate, (rate & 1u) != 0u, false) &&
			      geometry.four_times_gain_q0_16 == UINT16_MAX &&
			      v92_cpd_parameters_are_valid(&geometry),
			      "rate %u law %u geometry was not a fresh design seed", rate, law);
			geometry.four_times_gain_q0_16 = cpd.four_times_gain_q0_16;
			CHECK(memcmp(&geometry, &cpd, sizeof(cpd)) == 0,
			      "rate %u law %u geometry changed normalized parameters", rate, law);
			CHECK(cpd.data_rate_number == rate &&
			      cpd.extend_e2u == ((rate & 1u) != 0u) &&
			      !cpd.acknowledges_cpu &&
			      cpd.trellis ==
				      (rate == 13u ?
					       V92_CPD_TRELLIS_32_STATE :
					       V92_CPD_TRELLIS_16_STATE),
			      "rate %u law %u fixed selection changed",
			      rate, law);
			CHECK(direct_sections_are_correct(
				      &cpd, (enum g711_law)law, rate) &&
			      gain_is_nearest(&cpd, (enum g711_law)law),
			      "rate %u law %u direct mapping changed",
			      rate, law);
			for (uint8_t alignment =
				     V92_CPD_DOWNSTREAM_FRAME_MIN_BITS;
			     alignment <= V92_CPD_DOWNSTREAM_FRAME_MAX_BITS;
			     ++alignment) {
				struct v92_cpd_parameters decoded;
				size_t expected_bits =
					v92_cpd_frame_bits(&cpd, alignment);
				size_t frame_bytes = (expected_bits + 7u) / 8u;
				size_t frame_bits = 0u;

				memset(frame, 0xa5, sizeof(frame));
				CHECK(expected_bits != 0u &&
				      expected_bits <=
					      V92_CPD_DIRECT_MAX_FRAME_BITS &&
				      frame_bytes <= sizeof(frame) &&
				      v92_cpd_encode(
					      &cpd, alignment, frame,
					      frame_bytes, &frame_bits) ==
					      V92_CPD_OK &&
				      frame_bits == expected_bits &&
				      v92_cpd_decode(
					      frame, frame_bytes, frame_bits,
					      alignment, &decoded) == V92_CPD_OK &&
				      memcmp(&decoded, &cpd, sizeof(cpd)) == 0,
				      "rate %u law %u alignment %u CPd "
				      "did not round trip within its bound",
				      rate, law, alignment);
			}
		}
	}
	return true;
}

static bool test_known_boundaries(void)
{
	struct v92_cpd_parameters cpd;

	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMU, 1u, false) == V92_CPD_OK &&
	      cpd.four_times_gain_q0_16 == 165u,
	      "DRN1 gain was %u rather than 165", cpd.four_times_gain_q0_16);
	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMA, 4u, true) == V92_CPD_OK &&
	      cpd.four_times_gain_q0_16 == 108u &&
	      v92_cpd_frame_bits(&cpd, 17u) == 680u,
	      "DRN4 direct CPd changed gain or wire extent");
	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMU, 13u, false) == V92_CPD_OK &&
	      cpd.trellis == V92_CPD_TRELLIS_32_STATE &&
	      cpd.constellation[0].count == V92_CPD_DIRECT_MAX_POINTS,
	      "DRN13 did not select its expanded 32-state mapping");
	struct wide_uint smaller = wide_from_u64(1);

	for (unsigned symbol = 0u; symbol < V92_CPD_MODULUS_COUNT; ++symbol)
		wide_multiply_u32(smaller, symbol % 4u == 3u ? 26u : 52u, &smaller);
	CHECK(wide_compare(smaller, wide_shift_left(wide_from_u64(1), 66)) < 0,
	      "44k allocation is not the smallest even, uniform signed alphabet");
	return true;
}

static bool test_transactionality(void)
{
	struct v92_cpd_parameters cpd;
	struct v92_cpd_parameters before;

	memset(&cpd, 0x5a, sizeof(cpd));
	before = cpd;
	CHECK(!v92_cpd_direct_geometry(NULL, G711_LAW_PCMU, 1u, false, false) &&
	      !v92_cpd_direct_geometry(&cpd, G711_LAW_PCMU, 0u, false, false) &&
	      !v92_cpd_direct_geometry(&cpd, G711_LAW_PCMU,
		      V92_CPD_MAX_DATA_RATE_NUMBER + 1u, false, false) &&
	      !v92_cpd_direct_geometry(&cpd, (enum g711_law)99, 1u, false, false) &&
	      memcmp(&cpd, &before, sizeof(cpd)) == 0,
	      "invalid geometry changed its owner");
	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMU, 0u, false) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      memcmp(&cpd, &before, sizeof(cpd)) == 0,
	      "cleardown changed the rejected output");
	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMU,
		      V92_CPD_MAX_DATA_RATE_NUMBER + 1u, false) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      memcmp(&cpd, &before, sizeof(cpd)) == 0,
	      "unsupported rate changed the rejected output");
	CHECK(v92_cpd_direct_init(
		      &cpd, (enum g711_law)99, 1u, false) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      memcmp(&cpd, &before, sizeof(cpd)) == 0 &&
	      v92_cpd_direct_init(
		      NULL, G711_LAW_PCMU, 1u, false) ==
		      V92_CPD_INVALID_ARGUMENT,
	      "invalid arguments were accepted");
	return true;
}

static bool quantized_points_are_distinct(
	const struct v92_cpd_parameters *cpd, enum g711_law law, int16_t lu)
{
	struct v92_pcm_mapping mapping;
	const struct v92_cpd_constellation *points = &cpd->constellation[0];
	int previous = INT16_MIN;

	CHECK(v92_pcm_mapping_init(&mapping, law, cpd) == V92_PCM_MAPPING_OK,
	      "selected mapping was invalid");
	for (int eta = -(int)points->count; eta < points->count; ++eta) {
		int u = eta < 0 ? -points->positive_linear[-eta - 1] :
			points->positive_linear[eta];
		int32_t v = round_shift_s64_to_s32_saturating((int64_t)u * INT16_MAX, 15u);
		int16_t linear;
		int sample;

		CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, lu, v, &linear) == V92_PCM_FRAME_OK &&
		      linear > INT16_MIN && linear < INT16_MAX, "selected point clipped");
		sample = law == G711_LAW_PCMA ?
			g711_decode_pcma(g711_encode_pcma(linear)) :
			g711_decode_pcmu(g711_encode_pcmu(linear));
		CHECK(sample > previous, "law %u LU %d rate %u point %d collapsed",
		      law, lu, cpd->data_rate_number, eta);
		previous = sample;
	}
	return true;
}

static bool test_level_selection(void)
{
	static const int16_t levels[] = { 4092, 4096, 4224, 5987, 7040, 7139, 7164 };
	static const uint8_t rates[] = { 1, 4, 13, 15, 16, 17, 18, 19 };

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t level = 0u; level < ARRAY_LENGTH(levels); ++level)
			for (size_t rate = 0u; rate < ARRAY_LENGTH(rates); ++rate) {
				struct v92_cpd_parameters cpd, decoded;
				uint8_t wire[V92_CPD_DIRECT_MAX_FRAME_BYTES];
				size_t bits;

				CHECK(v92_cpd_direct_select(&cpd, law, rates[rate], levels[level], true) ==
					      V92_CPD_OK && cpd.data_rate_number == rates[rate] &&
				      cpd.extend_e2u && v92_cpd_parameters_are_valid(&cpd) &&
				      gain_is_nearest(&cpd, law) &&
				      quantized_points_are_distinct(&cpd, law, levels[level]),
				      "law %u LU %d rate %u selection failed", law, levels[level], rates[rate]);
				if (rates[rate] >= 15u && rates[rate] <= 19u) {
					static const uint8_t minimum_points[] = { 24u, 27u, 31u, 34u, 39u };
					unsigned points = cpd.constellation[0].count;
					struct wide_uint capacity = wide_from_u64(1), smaller = wide_from_u64(1);
					struct wide_uint required = wide_shift_left(wide_from_u64(1), pcm_upstream_frame_bits(rates[rate]));

					CHECK(points == minimum_points[rates[rate] - 15u],
					      "direct selection retained an oversized alphabet");
					for (size_t symbol = 0u; symbol < V92_CPD_MODULUS_COUNT; ++symbol) {
						unsigned factor = symbol % 4u == 3u ? 1u : 2u;

						CHECK(cpd.modulus[symbol] == factor * points,
						      "direct selection did not balance its intervals");
						wide_multiply_u32(capacity, cpd.modulus[symbol], &capacity);
						wide_multiply_u32(smaller, factor * (points - 1u), &smaller);
					}
					CHECK(wide_compare(smaller, required) < 0 && wide_compare(capacity, required) >= 0,
					      "balanced alphabet was not minimal or lost data capacity");
					struct g711_interval interval;

					CHECK(g711_quantization_interval(law, levels[level], &interval),
					      "training level has no quantization interval");
					for (int actual = interval.lower; actual <= interval.upper; ++actual)
						CHECK(quantized_points_are_distinct(&cpd, law, (int16_t)actual),
						      "balanced selection collapsed within training uncertainty");
				}
				CHECK(v92_cpd_encode(&cpd, 17u, wire, sizeof(wire), &bits) == V92_CPD_OK &&
				      v92_cpd_decode(wire, (bits + 7u) / 8u, bits, 17u, &decoded) == V92_CPD_OK &&
				      memcmp(&cpd, &decoded, sizeof(cpd)) == 0,
				      "selected CPd did not survive wire encoding");
			}
	struct v92_cpd_parameters cpd, before;

	CHECK(v92_cpd_direct_init(&before, G711_LAW_PCMU, 4u, false) == V92_CPD_OK &&
	      v92_cpd_direct_select(&cpd, G711_LAW_PCMU, 4u, 4092, false) == V92_CPD_OK &&
	      memcmp(&cpd, &before, sizeof(cpd)) == 0, "usable template changed");
	CHECK(v92_cpd_direct_select(&cpd, G711_LAW_PCMA, 19u, 1, false) ==
		      V92_CPD_INVALID_PARAMETERS && memcmp(&cpd, &before, sizeof(cpd)) == 0,
	      "unusable level was accepted or changed output");
	CHECK(v92_cpd_direct_select(&cpd, G711_LAW_PCMA, 0u, 7040, false) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      v92_cpd_direct_select(&cpd, G711_LAW_PCMA, 19u, 0, false) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      v92_cpd_direct_select(&cpd, (enum g711_law)99, 19u, 7040, false) ==
		      V92_CPD_INVALID_PARAMETERS && memcmp(&cpd, &before, sizeof(cpd)) == 0 &&
	      v92_cpd_direct_select(NULL, G711_LAW_PCMA, 19u, 7040, false) ==
		      V92_CPD_INVALID_ARGUMENT, "invalid level-selection arguments accepted");
	for (uint8_t rate = 15u; rate <= 19u; ++rate)
		CHECK(v92_cpd_direct_select(&cpd, G711_LAW_PCMA, rate, 1, false) ==
			      V92_CPD_INVALID_PARAMETERS && memcmp(&cpd, &before, sizeof(cpd)) == 0,
		      "unusable balanced selection changed output");
	return true;
}

static bool test_low_rate_cell_selection(void)
{
	static const struct {
		enum g711_law law;
		uint8_t rate, ucode;
	} cells[] = {
		{ G711_LAW_PCMU, 13u, 80u },
		{ G711_LAW_PCMA, 13u, 37u },
		{ G711_LAW_PCMA, 13u, 80u },
		{ G711_LAW_PCMA, 13u, 96u },
		{ G711_LAW_PCMU, 14u, 26u },
		{ G711_LAW_PCMA, 14u, 36u },
	};

	/* Nominally distinct maps can alias elsewhere in the reported cell.
	 * Check native scaling and the actual codec throughout each witness;
	 * any improved constellation with these properties is acceptable. */
	for (size_t i = 0u; i < ARRAY_LENGTH(cells); ++i) {
		struct v92_cpd_parameters selected;
		struct g711_interval levels;
		int16_t nominal;

		CHECK(g711_ucode_sample(cells[i].law, cells[i].ucode, true, &nominal) &&
		      g711_quantization_interval(cells[i].law, nominal, &levels) &&
		      v92_cpd_direct_select(&selected, cells[i].law, cells[i].rate,
			      nominal, false) == V92_CPD_OK &&
		      v92_cpd_parameters_are_valid(&selected) &&
		      gain_is_nearest(&selected, cells[i].law),
		      "low-rate training-cell selection failed");
		for (int level = levels.lower; level <= levels.upper; ++level)
			CHECK(quantized_points_are_distinct(&selected, cells[i].law,
				      (int16_t)level),
			      "low-rate alphabet aliases within its reported cell");
	}
	/* Failure to find a scalar full-cell margin must retain an existing
	 * nominally usable coded profile, whose B1u still checks reception. */
	struct v92_cpd_parameters fallback;
	int16_t nominal;

	CHECK(g711_ucode_sample(G711_LAW_PCMU, 16u, true, &nominal) &&
	      v92_cpd_direct_select(&fallback, G711_LAW_PCMU, 13u, nominal, false) == V92_CPD_OK &&
	      v92_cpd_parameters_are_valid(&fallback) &&
	      quantized_points_are_distinct(&fallback, G711_LAW_PCMU, nominal),
	      "nominal low-rate fallback was lost");
	return true;
}

static bool test_full_cell_selection(void)
{
	/* Inspect actual native transmitter samples at every LU in each codec
	 * cell. A fast score must never skip a zero-margin transition or accept
	 * a clipping alphabet. Rejection is allowed; valid improved maps remain
	 * usable without pinning the selector to one historical constellation. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 15u; rate <= 19u; ++rate) {
			unsigned accepted = 0u;

			for (unsigned code = 0u; code < 128u; ++code) {
				struct v92_cpd_parameters selected, before;
				struct g711_interval levels;
				int16_t nominal;

				CHECK(g711_ucode_sample(law, (uint8_t)code, true, &nominal) &&
				      g711_quantization_interval(law, nominal, &levels),
				      "training-cell fixture failed");
				memset(&selected, 0xa5, sizeof(selected));
				before = selected;
				enum v92_cpd_result result = v92_cpd_direct_select(
					&selected, law, rate, nominal, (code & 1u) != 0u);

				if (result == V92_CPD_INVALID_PARAMETERS) {
					CHECK(memcmp(&selected, &before, sizeof(selected)) == 0,
					      "rejected training cell changed output");
					continue;
				}
				CHECK(result == V92_CPD_OK && levels.lower > 0 &&
				      v92_cpd_parameters_are_valid(&selected),
				      "invalid training cell was selected");
				const struct v92_cpd_constellation *points = &selected.constellation[0];
				int32_t filtered[2u * V92_CPD_DIRECT_MAX_POINTS];
				int count = points->count;

				for (int eta = -count; eta < count; ++eta) {
					int u = eta < 0 ? -points->positive_linear[-eta - 1] :
						points->positive_linear[eta];

					filtered[eta + count] = round_shift_s64_to_s32_saturating(
						(int64_t)u * INT16_MAX, 15u);
				}
				for (int lu = levels.lower; lu <= levels.upper; ++lu) {
					int previous = INT16_MIN;

					for (int index = 0; index < 2 * count; ++index) {
						int16_t linear;

						CHECK(v92_pcm_frame_scale(selected.four_times_gain_q0_16,
							      (int16_t)lu, filtered[index], &linear) == V92_PCM_FRAME_OK &&
						      linear > INT16_MIN && linear < INT16_MAX,
						      "law %u rate %u code %u LU %d clipped", law, rate, code, lu);
						int reconstructed = g711_quantize(law, linear);

						CHECK(reconstructed > previous,
						      "law %u rate %u code %u LU %d point %d collapsed",
						      law, rate, code, lu, index);
						previous = reconstructed;
					}
				}
				accepted++;
			}
			CHECK(accepted != 0u, "law %u rate %u rejected every training cell", law, rate);
		}
	}
	return true;
}

static bool test_uncertain_symbol_ranges(void)
{
	/* At ordinary direct levels a 46 2/3k alphabet can keep neighboring
	 * symbols distinct without first knowing LU's position in its cell. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		int16_t lu = law == G711_LAW_PCMU ? 6908 : 7040;
		struct g711_interval interval;
		struct v92_cpd_parameters cpd;
		struct v92_pcm_mapping mapping;
		int previous_maximum = INT16_MIN;

		CHECK(g711_quantization_interval(law, lu, &interval) &&
		      v92_cpd_direct_select(&cpd, law, 18u, lu, false) == V92_CPD_OK &&
		      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK,
		      "uncertain direct mapping did not initialize");
		const struct v92_cpd_constellation *points = &cpd.constellation[0];
		for (int eta = -(int)points->count; eta < points->count; ++eta) {
			int minimum = INT16_MAX, maximum = INT16_MIN;
			int u = eta < 0 ? -points->positive_linear[-eta - 1] : points->positive_linear[eta];
			int32_t v = round_shift_s64_to_s32_saturating((int64_t)u * INT16_MAX, 15u);

			for (int level = interval.lower; level <= interval.upper; ++level) {
				int16_t linear;

				CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, (int16_t)level, v, &linear) == V92_PCM_FRAME_OK,
				      "uncertain symbol clipped");
				int decoded = law == G711_LAW_PCMU ?
					g711_decode_pcmu(g711_encode_pcmu(linear)) :
					g711_decode_pcma(g711_encode_pcma(linear));

				if (decoded < minimum)
					minimum = decoded;
				if (decoded > maximum)
					maximum = decoded;
			}
			CHECK(minimum > previous_maximum,
			      "law %u point %d overlaps its neighbor under LU uncertainty", law, eta);
			previous_maximum = maximum;
		}
	}
	return true;
}

static bool check_input_noise_margin(
	enum g711_law law, uint8_t rate, int16_t nominal, int noise)
{
	/* Check the wire cells independently of the selector's score. At each
	 * possible training level, even opposite peak perturbations must leave
	 * adjacent symbols distinct. Do not require different LU hypotheses
	 * to separate: known B1u resolves that uncertainty before payload. */

	struct g711_interval interval;
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;

	CHECK(g711_quantization_interval(law, nominal, &interval) &&
	      v92_cpd_direct_select(&cpd, law, rate, nominal, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      gain_is_nearest(&cpd, law),
	      "rate %u law %u noise-margin mapping failed", rate, law);
	const struct v92_cpd_constellation *points = &cpd.constellation[0];
	for (int lu = interval.lower; lu <= interval.upper; ++lu) {
		int previous_maximum = INT16_MIN;

		for (int eta = -(int)points->count; eta < points->count; ++eta) {
			int u = eta < 0 ? -points->positive_linear[-eta - 1] :
				points->positive_linear[eta];
			int32_t v = round_shift_s64_to_s32_saturating((int64_t)u * INT16_MAX, 15u);
			int16_t linear;

			CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, (int16_t)lu, v, &linear) == V92_PCM_FRAME_OK &&
			      linear - noise >= INT16_MIN &&
			      linear + noise <= INT16_MAX, "rate %u law %u symbol clipped", rate, law);
			int minimum = law == G711_LAW_PCMU ?
				g711_decode_pcmu(g711_encode_pcmu(linear - noise)) :
				g711_decode_pcma(g711_encode_pcma(linear - noise));
			int maximum = law == G711_LAW_PCMU ?
				g711_decode_pcmu(g711_encode_pcmu(linear + noise)) :
				g711_decode_pcma(g711_encode_pcma(linear + noise));

			CHECK(minimum > previous_maximum,
			      "rate %u law %u LU %d eta %d lost input-noise margin", rate, law, lu, eta);
			previous_maximum = maximum;
		}
	}
	return true;
}

static bool test_input_noise_margin(void)
{
	const struct { int16_t lu; int noise; } high_rate[] = {
		{ 4091, 16 }, { 5471, 32 }, { 7040, 32 }, { 7777, 32 },
	};
	const struct { enum g711_law law; uint8_t rate, code; int noise; } balanced[] = {
		{ G711_LAW_PCMU, 15, 56, 16 },
		{ G711_LAW_PCMU, 16, 56, 12 },
		{ G711_LAW_PCMU, 17, 56, 8 },
		{ G711_LAW_PCMA, 15, 48, 12 },
		{ G711_LAW_PCMA, 16, 48, 8 },
		{ G711_LAW_PCMA, 17, 50, 4 },
	};

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t i = 0; i < ARRAY_LENGTH(high_rate); ++i)
			CHECK(check_input_noise_margin(law, 19u, high_rate[i].lu, high_rate[i].noise),
			      "48k noise witness failed");
	/* These balanced mappings used to have distinct decoded levels but
	 * little pre-codec noise headroom. Exercise both signs, all levels in
	 * the reported LU cell, and the actual transmitter's fixed scaling. */
	for (size_t i = 0; i < ARRAY_LENGTH(balanced); ++i) {
		int16_t nominal;

		CHECK(g711_ucode_sample(balanced[i].law, balanced[i].code, true, &nominal) &&
		      check_input_noise_margin(balanced[i].law, balanced[i].rate,
			      nominal, balanced[i].noise), "balanced noise witness %zu failed", i);
	}
	return true;
}

int main(void)
{
	if (!test_rate_and_law_matrix() || !test_known_boundaries() ||
	    !test_transactionality() || !test_level_selection() ||
	    !test_low_rate_cell_selection() || !test_full_cell_selection() ||
	    !test_uncertain_symbol_ranges() ||
	    !test_input_noise_margin())
		return 1;
	return 0;
}
