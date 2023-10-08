/* SPDX-License-Identifier: MIT */
/* Copyright © 2022-present Max Bachmann */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <rapidfuzz/details/PatternMatchVector.hpp>
#include <rapidfuzz/details/common.hpp>
#include <rapidfuzz/details/distance.hpp>
#include <rapidfuzz/details/intrinsics.hpp>
#include <vector>

namespace rapidfuzz::detail {

struct FlaggedCharsWord {
    uint64_t P_flag;
    uint64_t T_flag;
};

struct FlaggedCharsMultiword {
    std::vector<uint64_t> P_flag;
    std::vector<uint64_t> T_flag;
};

struct SearchBoundMask {
    size_t words = 0;
    size_t empty_words = 0;
    uint64_t last_mask = 0;
    uint64_t first_mask = 0;
};

struct TextPosition {
    TextPosition(int64_t Word_, int64_t WordPos_) : Word(Word_), WordPos(WordPos_)
    {}
    int64_t Word;
    int64_t WordPos;
};

static inline double jaro_calculate_similarity(int64_t P_len, int64_t T_len, size_t CommonChars,
                                               size_t Transpositions)
{
    Transpositions /= 2;
    double Sim = 0;
    Sim += static_cast<double>(CommonChars) / static_cast<double>(P_len);
    Sim += static_cast<double>(CommonChars) / static_cast<double>(T_len);
    Sim += (static_cast<double>(CommonChars) - static_cast<double>(Transpositions)) /
           static_cast<double>(CommonChars);
    return Sim / 3.0;
}

/**
 * @brief filter matches below score_cutoff based on string lengths
 */
static inline bool jaro_length_filter(int64_t P_len, int64_t T_len, double score_cutoff)
{
    if (!T_len || !P_len) return false;

    double min_len = static_cast<double>(std::min(P_len, T_len));
    double Sim = min_len / static_cast<double>(P_len) + min_len / static_cast<double>(T_len) + 1.0;
    Sim /= 3.0;
    return Sim >= score_cutoff;
}

/**
 * @brief filter matches below score_cutoff based on string lengths and common characters
 */
static inline bool jaro_common_char_filter(int64_t P_len, int64_t T_len, size_t CommonChars,
                                           double score_cutoff)
{
    if (!CommonChars) return false;

    double Sim = 0;
    Sim += static_cast<double>(CommonChars) / static_cast<double>(P_len);
    Sim += static_cast<double>(CommonChars) / static_cast<double>(T_len);
    Sim += 1.0;
    Sim /= 3.0;
    return Sim >= score_cutoff;
}

static inline size_t count_common_chars(const FlaggedCharsWord& flagged)
{
    return static_cast<size_t>(popcount(flagged.P_flag));
}

static inline size_t count_common_chars(const FlaggedCharsMultiword& flagged)
{
    size_t CommonChars = 0;
    if (flagged.P_flag.size() < flagged.T_flag.size()) {
        for (uint64_t flag : flagged.P_flag) {
            CommonChars += static_cast<size_t>(popcount(flag));
        }
    }
    else {
        for (uint64_t flag : flagged.T_flag) {
            CommonChars += static_cast<size_t>(popcount(flag));
        }
    }
    return CommonChars;
}

template <typename PM_Vec, typename InputIt1, typename InputIt2>
FlaggedCharsWord flag_similar_characters_word(const PM_Vec& PM, [[maybe_unused]] Range<InputIt1> P,
                                              Range<InputIt2> T, int Bound)
{
    assert(P.size() <= 64);
    assert(T.size() <= 64);
    assert(Bound > P.size() || P.size() - Bound <= T.size());

    FlaggedCharsWord flagged = {0, 0};

    uint64_t BoundMask = bit_mask_lsb<uint64_t>(Bound + 1);

    int64_t j = 0;
    for (; j < std::min(static_cast<int64_t>(Bound), static_cast<int64_t>(T.size())); ++j) {
        uint64_t PM_j = PM.get(0, T[j]) & BoundMask & (~flagged.P_flag);

        flagged.P_flag |= blsi(PM_j);
        flagged.T_flag |= static_cast<uint64_t>(PM_j != 0) << j;

        BoundMask = (BoundMask << 1) | 1;
    }

    for (; j < T.size(); ++j) {
        uint64_t PM_j = PM.get(0, T[j]) & BoundMask & (~flagged.P_flag);

        flagged.P_flag |= blsi(PM_j);
        flagged.T_flag |= static_cast<uint64_t>(PM_j != 0) << j;

        BoundMask <<= 1;
    }

    return flagged;
}

template <typename CharT>
void flag_similar_characters_step(const BlockPatternMatchVector& PM, CharT T_j,
                                  FlaggedCharsMultiword& flagged, size_t j, SearchBoundMask BoundMask)
{
    size_t j_word = j / 64;
    size_t j_pos = j % 64;
    size_t word = BoundMask.empty_words;
    size_t last_word = word + BoundMask.words;

    if (BoundMask.words == 1) {
        uint64_t PM_j =
            PM.get(word, T_j) & BoundMask.last_mask & BoundMask.first_mask & (~flagged.P_flag[word]);

        flagged.P_flag[word] |= blsi(PM_j);
        flagged.T_flag[j_word] |= static_cast<uint64_t>(PM_j != 0) << j_pos;
        return;
    }

    if (BoundMask.first_mask) {
        uint64_t PM_j = PM.get(word, T_j) & BoundMask.first_mask & (~flagged.P_flag[word]);

        if (PM_j) {
            flagged.P_flag[word] |= blsi(PM_j);
            flagged.T_flag[j_word] |= 1ull << j_pos;
            return;
        }
        word++;
    }

    /* unroll for better performance on long sequences when access is fast */
    if (T_j >= 0 && T_j < 256) {
        for (; word + 3 < last_word - 1; word += 4) {
            uint64_t PM_j[4];
            unroll<int, 4>([&](auto i) {
                PM_j[i] = PM.get(word + i, static_cast<uint8_t>(T_j)) & (~flagged.P_flag[word + i]);
            });

            if (PM_j[0]) {
                flagged.P_flag[word] |= blsi(PM_j[0]);
                flagged.T_flag[j_word] |= 1ull << j_pos;
                return;
            }
            if (PM_j[1]) {
                flagged.P_flag[word + 1] |= blsi(PM_j[1]);
                flagged.T_flag[j_word] |= 1ull << j_pos;
                return;
            }
            if (PM_j[2]) {
                flagged.P_flag[word + 2] |= blsi(PM_j[2]);
                flagged.T_flag[j_word] |= 1ull << j_pos;
                return;
            }
            if (PM_j[3]) {
                flagged.P_flag[word + 3] |= blsi(PM_j[3]);
                flagged.T_flag[j_word] |= 1ull << j_pos;
                return;
            }
        }
    }

    for (; word < last_word - 1; ++word) {
        uint64_t PM_j = PM.get(word, T_j) & (~flagged.P_flag[word]);

        if (PM_j) {
            flagged.P_flag[word] |= blsi(PM_j);
            flagged.T_flag[j_word] |= 1ull << j_pos;
            return;
        }
    }

    if (BoundMask.last_mask) {
        uint64_t PM_j = PM.get(word, T_j) & BoundMask.last_mask & (~flagged.P_flag[word]);

        flagged.P_flag[word] |= blsi(PM_j);
        flagged.T_flag[j_word] |= static_cast<uint64_t>(PM_j != 0) << j_pos;
    }
}

template <typename InputIt1, typename InputIt2>
static inline FlaggedCharsMultiword flag_similar_characters_block(const BlockPatternMatchVector& PM,
                                                                  Range<InputIt1> P, Range<InputIt2> T,
                                                                  int64_t Bound)
{
    assert(P.size() > 64 || T.size() > 64);
    assert(Bound > P.size() || P.size() - Bound <= T.size());
    assert(Bound >= 31);

    FlaggedCharsMultiword flagged;
    flagged.T_flag.resize(static_cast<size_t>(ceil_div(T.size(), 64)));
    flagged.P_flag.resize(static_cast<size_t>(ceil_div(P.size(), 64)));

    SearchBoundMask BoundMask;
    size_t start_range = static_cast<size_t>(std::min(Bound + 1, static_cast<int64_t>(P.size())));
    BoundMask.words = 1 + start_range / 64;
    BoundMask.empty_words = 0;
    BoundMask.last_mask = (1ull << (start_range % 64)) - 1;
    BoundMask.first_mask = ~UINT64_C(0);

    for (int64_t j = 0; j < T.size(); ++j) {
        flag_similar_characters_step(PM, T[j], flagged, static_cast<size_t>(j), BoundMask);

        if (j + Bound + 1 < P.size()) {
            BoundMask.last_mask = (BoundMask.last_mask << 1) | 1;
            if (j + Bound + 2 < P.size() && BoundMask.last_mask == ~UINT64_C(0)) {
                BoundMask.last_mask = 0;
                BoundMask.words++;
            }
        }

        if (j >= Bound) {
            BoundMask.first_mask <<= 1;
            if (BoundMask.first_mask == 0) {
                BoundMask.first_mask = ~UINT64_C(0);
                BoundMask.words--;
                BoundMask.empty_words++;
            }
        }
    }

    return flagged;
}

template <typename PM_Vec, typename InputIt1>
static inline size_t count_transpositions_word(const PM_Vec& PM, Range<InputIt1> T,
                                               const FlaggedCharsWord& flagged)
{
    uint64_t P_flag = flagged.P_flag;
    uint64_t T_flag = flagged.T_flag;
    size_t Transpositions = 0;
    while (T_flag) {
        uint64_t PatternFlagMask = blsi(P_flag);

        Transpositions += !(PM.get(0, T[countr_zero(T_flag)]) & PatternFlagMask);

        T_flag = blsr(T_flag);
        P_flag ^= PatternFlagMask;
    }

    return Transpositions;
}

template <typename InputIt1>
static inline size_t count_transpositions_block(const BlockPatternMatchVector& PM, Range<InputIt1> T,
                                                const FlaggedCharsMultiword& flagged, size_t FlaggedChars)
{
    size_t TextWord = 0;
    size_t PatternWord = 0;
    uint64_t T_flag = flagged.T_flag[TextWord];
    uint64_t P_flag = flagged.P_flag[PatternWord];

    auto T_first = T.begin();
    size_t Transpositions = 0;
    while (FlaggedChars) {
        while (!T_flag) {
            TextWord++;
            T_first += 64;
            T_flag = flagged.T_flag[TextWord];
        }

        while (T_flag) {
            while (!P_flag) {
                PatternWord++;
                P_flag = flagged.P_flag[PatternWord];
            }

            uint64_t PatternFlagMask = blsi(P_flag);

            Transpositions += !(PM.get(PatternWord, T_first[countr_zero(T_flag)]) & PatternFlagMask);

            T_flag = blsr(T_flag);
            P_flag ^= PatternFlagMask;

            FlaggedChars--;
        }
    }

    return Transpositions;
}

// todo cleanup the split between jaro_bounds
/**
 * @brief find bounds
 */
static inline int64_t jaro_bounds(int64_t P_len, int64_t T_len)
{
    /* since jaro uses a sliding window some parts of T/P might never be in
     * range an can be removed ahead of time
     */
    int64_t Bound = 0;
    if (T_len > P_len) {
        Bound = T_len / 2 - 1;
    }
    else {
        Bound = P_len / 2 - 1;
    }
    return Bound;
}

/**
 * @brief find bounds and skip out of bound parts of the sequences
 */
template <typename InputIt1, typename InputIt2>
int64_t jaro_bounds(Range<InputIt1>& P, Range<InputIt2>& T)
{
    int64_t P_len = P.size();
    int64_t T_len = T.size();

    /* since jaro uses a sliding window some parts of T/P might never be in
     * range an can be removed ahead of time
     */
    int64_t Bound = 0;
    if (T_len > P_len) {
        Bound = T_len / 2 - 1;
        if (T_len > P_len + Bound) T.remove_suffix(T_len - (P_len + Bound));
    }
    else {
        Bound = P_len / 2 - 1;
        if (P_len > T_len + Bound) P.remove_suffix(P_len - (T_len + Bound));
    }
    return Bound;
}

template <typename InputIt1, typename InputIt2>
double jaro_similarity(Range<InputIt1> P, Range<InputIt2> T, double score_cutoff)
{
    int64_t P_len = P.size();
    int64_t T_len = T.size();

    if (score_cutoff > 1.0) return 0.0;

    if (!P_len && !T_len) return 1.0;

    /* filter out based on the length difference between the two strings */
    if (!jaro_length_filter(P_len, T_len, score_cutoff)) return 0.0;

    if (P_len == 1 && T_len == 1) return static_cast<double>(P[0] == T[0]);

    int64_t Bound = jaro_bounds(P, T);

    /* common prefix never includes Transpositions */
    size_t CommonChars = remove_common_prefix(P, T);
    size_t Transpositions = 0;

    if (P.empty() || T.empty()) {
        /* already has correct number of common chars and transpositions */
    }
    else if (P.size() <= 64 && T.size() <= 64) {
        PatternMatchVector PM(P);
        auto flagged = flag_similar_characters_word(PM, P, T, static_cast<int>(Bound));
        CommonChars += count_common_chars(flagged);

        if (!jaro_common_char_filter(P_len, T_len, CommonChars, score_cutoff)) return 0.0;

        Transpositions = count_transpositions_word(PM, T, flagged);
    }
    else {
        BlockPatternMatchVector PM(P);
        auto flagged = flag_similar_characters_block(PM, P, T, Bound);
        size_t FlaggedChars = count_common_chars(flagged);
        CommonChars += FlaggedChars;

        if (!jaro_common_char_filter(P_len, T_len, CommonChars, score_cutoff)) return 0.0;

        Transpositions = count_transpositions_block(PM, T, flagged, FlaggedChars);
    }

    double Sim = jaro_calculate_similarity(P_len, T_len, CommonChars, Transpositions);
    return (Sim >= score_cutoff) ? Sim : 0;
}

template <typename InputIt1, typename InputIt2>
double jaro_similarity(const BlockPatternMatchVector& PM, Range<InputIt1> P, Range<InputIt2> T,
                       double score_cutoff)
{
    int64_t P_len = P.size();
    int64_t T_len = T.size();

    if (score_cutoff > 1.0) return 0.0;

    if (!P_len && !T_len) return 1.0;

    /* filter out based on the length difference between the two strings */
    if (!jaro_length_filter(P_len, T_len, score_cutoff)) return 0.0;

    if (P_len == 1 && T_len == 1) return static_cast<double>(P[0] == T[0]);

    int64_t Bound = jaro_bounds(P, T);

    /* common prefix never includes Transpositions */
    size_t CommonChars = 0;
    size_t Transpositions = 0;

    if (P.empty() || T.empty()) {
        /* already has correct number of common chars and transpositions */
    }
    else if (P.size() <= 64 && T.size() <= 64) {
        auto flagged = flag_similar_characters_word(PM, P, T, static_cast<int>(Bound));
        CommonChars += count_common_chars(flagged);

        if (!jaro_common_char_filter(P_len, T_len, CommonChars, score_cutoff)) return 0.0;

        Transpositions = count_transpositions_word(PM, T, flagged);
    }
    else {
        auto flagged = flag_similar_characters_block(PM, P, T, Bound);
        size_t FlaggedChars = count_common_chars(flagged);
        CommonChars += FlaggedChars;

        if (!jaro_common_char_filter(P_len, T_len, CommonChars, score_cutoff)) return 0.0;

        Transpositions = count_transpositions_block(PM, T, flagged, FlaggedChars);
    }

    double Sim = jaro_calculate_similarity(P_len, T_len, CommonChars, Transpositions);
    return (Sim >= score_cutoff) ? Sim : 0;
}

#ifdef RAPIDFUZZ_SIMD
template <typename VecType, typename InputIt, int _lto_hack = RAPIDFUZZ_LTO_HACK>
void jaro_similarity_simd(Range<double*> scores, const detail::BlockPatternMatchVector& block,
                          const std::vector<int64_t>& s1_lengths, Range<InputIt> s2,
                          double score_cutoff) noexcept
{
#    ifdef RAPIDFUZZ_AVX2
    using namespace simd_avx2;
#    else
    using namespace simd_sse2;
#    endif

    static constexpr size_t vec_width = native_simd<VecType>::size();
    static constexpr size_t vecs = static_cast<size_t>(native_simd<uint64_t>::size());
    assert(block.size() % vecs == 0);

    native_simd<VecType> zero(VecType(0));
    native_simd<VecType> one(1);
    size_t result_index = 0;

    if (score_cutoff > 1.0) {
        for (int64_t i = 0; i < static_cast<int64_t>(s1_lengths.size()); i++)
            scores[i] = 0.0;

        return;
    }

    if (s2.empty()) {
        for (size_t i = 0; i < s1_lengths.size(); i++)
            scores[static_cast<int64_t>(i)] = s1_lengths[i] ? 0.0 : 1.0;

        return;
    }

    for (size_t cur_vec = 0; cur_vec < block.size(); cur_vec += vecs) {
        alignas(32) std::array<VecType, vec_width> boundMaskSize_;
        alignas(32) std::array<VecType, vec_width> boundMask_;

        auto s2_cur = s2;

        int64_t lastRelevantChar = 0;
        int64_t maxBound = 0;
        unroll<int, vec_width>([&](auto i) {
            int64_t s1_len = s1_lengths[result_index + i];
            int64_t Bound = jaro_bounds(s1_len, s2_cur.size());

            if (s1_len + Bound > lastRelevantChar) lastRelevantChar = s1_len + Bound;

            if (Bound > maxBound) maxBound = Bound;

            boundMaskSize_[i] = bit_mask_lsb<VecType>(static_cast<int>(2 * Bound));
            boundMask_[i] = bit_mask_lsb<VecType>(static_cast<int>(Bound + 1));
        });

        if (s2_cur.size() > lastRelevantChar) s2_cur.remove_suffix(s2_cur.size() - lastRelevantChar);

        native_simd<VecType> boundMaskSize(reinterpret_cast<uint64_t*>(boundMaskSize_.data()));
        native_simd<VecType> boundMask(reinterpret_cast<uint64_t*>(boundMask_.data()));

        native_simd<VecType> P_flag(VecType(0));
        native_simd<VecType> T_flag(VecType(0));

        native_simd<VecType> counter(VecType(1));

        for (const auto& ch : s2_cur) {
            alignas(32) std::array<uint64_t, vecs> stored;
            unroll<int, vecs>([&](auto i) { stored[i] = block.get(cur_vec + i, ch); });
            native_simd<VecType> X(stored.data());
            native_simd<VecType> PM_j = andnot(X & boundMask, P_flag);

            P_flag |= blsi(PM_j);
            T_flag |= andnot(counter, (PM_j == zero));

            counter = counter << 1;
            boundMask = (boundMask << 1) | ((boundMask <= boundMaskSize) & one);
        }

        auto counts = popcount(P_flag);
        alignas(32) std::array<VecType, vec_width> P_flags;
        P_flag.store(P_flags.data());
        alignas(32) std::array<VecType, vec_width> T_flags;
        T_flag.store(T_flags.data());
        for (size_t i = 0; i < vec_width; ++i) {
            VecType CommonChars = counts[i];
            if (!jaro_common_char_filter(s1_lengths[result_index], s2.size(), CommonChars, score_cutoff)) {
                scores[static_cast<int64_t>(result_index)] = 0.0;
                result_index++;
                continue;
            }

            VecType P_flag_cur = P_flags[i];
            VecType T_flag_cur = T_flags[i];
            size_t Transpositions = 0;

            static constexpr size_t vecs_per_word = vec_width / vecs;
            size_t cur_block = i / vecs_per_word;
            int64_t offset = static_cast<int64_t>(sizeof(VecType) * 8 * (i % vecs_per_word));
            while (T_flag_cur) {
                VecType PatternFlagMask = blsi(P_flag_cur);

                uint64_t PM_j = block.get(cur_block, s2[countr_zero(T_flag_cur)]);
                Transpositions += !(PM_j & (static_cast<uint64_t>(PatternFlagMask) << offset));

                T_flag_cur = blsr(T_flag_cur);
                P_flag_cur ^= PatternFlagMask;
            }

            double Sim =
                jaro_calculate_similarity(s1_lengths[result_index], s2.size(), CommonChars, Transpositions);

            scores[static_cast<int64_t>(result_index)] = (Sim >= score_cutoff) ? Sim : 0;
            result_index++;
        }
    }
}
#endif /* RAPIDFUZZ_SIMD */

class Jaro : public SimilarityBase<Jaro, double, 0, 1> {
    friend SimilarityBase<Jaro, double, 0, 1>;
    friend NormalizedMetricBase<Jaro>;

    template <typename InputIt1, typename InputIt2>
    static double maximum(Range<InputIt1>, Range<InputIt2>) noexcept
    {
        return 1.0;
    }

    template <typename InputIt1, typename InputIt2>
    static double _similarity(Range<InputIt1> s1, Range<InputIt2> s2, double score_cutoff,
                              [[maybe_unused]] double score_hint)
    {
        return jaro_similarity(s1, s2, score_cutoff);
    }
};

} // namespace rapidfuzz::detail
