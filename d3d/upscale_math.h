#pragma once

// The game-independent arithmetic of temporal upscaling (d3d/upscale.h): the jitter sequence and the camera's motion
// found from the draws' own position matrices. No GPU, no guest memory: d3d/upscale_test.cpp checks it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace upscale
{
    using Matrix = std::array<double, 16>; // row-major, for column vectors: clip = M x position

    constexpr Matrix Identity{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    // The radical inverse of `index` in `base` (Halton): a low-discrepancy sequence in [0, 1).
    inline double Halton(uint32_t index, uint32_t base)
    {
        double result = 0.0, fraction = 1.0;
        while (index > 0)
        {
            fraction /= base;
            result += fraction * (index % base);
            index /= base;
        }
        return result;
    }

    // Jitter phases for an upscale of `ratio` per side: 8 x ratio squared (the advice of AMD's FSR and Apple's MetalFX), so
    // that every output pixel gets several distinct input samples.
    inline uint32_t JitterPhases(float ratio) { return std::max(8u, uint32_t(std::ceil(8.0f * ratio * ratio))); }

    // The sub-pixel offset of frame `frame`, in input pixels, each axis in [-0.5, 0.5): Halton (2, 3), from index 1.
    inline std::array<float, 2> Jitter(uint64_t frame, uint32_t phases)
    {
        const uint32_t index = uint32_t(frame % phases) + 1;
        return { float(Halton(index, 2) - 0.5), float(Halton(index, 3) - 0.5) };
    }

    // Four float4 constants c0-c3 holding a position matrix in the row-vector convention (clip = x c0 + y c1 + z c2 + c3,
    // [Alchemy]) as a column-vector matrix.
    inline Matrix FromRowVectors(const float rows[16])
    {
        Matrix m;
        for (uint32_t i = 0; i < 4; i++)
            for (uint32_t j = 0; j < 4; j++)
                m[j * 4 + i] = rows[i * 4 + j];
        return m;
    }

    // A viewport depth range of the scene's depth target this frame and the draw weight (vertices) drawn with it.
    struct DepthRange
    {
        float minDepth, maxDepth;
        uint64_t weight;
    };

    // The range FSR's inputs pass maps the scene's window depth back to clip z with: the widest of the ranges holding at
    // least a quarter of the weight, else the widest of all. Not the widest alone: a sky draws at a single value (far
    // plane), and a game may draw its world in a slice of the range with light full-screen passes over all of it [Alchemy]
    // (Swap Force: the world in 1 -> 0.2 and depth-only quads in 1 -> 0; mapped with the quads' range, the far plane
    // came out at 0.8 and every pixel's motion was the clamp's).
    inline DepthRange SceneDepthRange(const DepthRange* ranges, uint32_t count)
    {
        uint64_t total = 0;
        for (uint32_t i = 0; i < count; i++)
            total += ranges[i].weight;
        const DepthRange* widest = nullptr;
        for (int pass = 0; pass < 2 && widest == nullptr; pass++)
            for (uint32_t i = 0; i < count; i++)
                if ((pass == 1 || ranges[i].weight * 4 >= total) &&
                    (widest == nullptr || std::fabs(ranges[i].maxDepth - ranges[i].minDepth) >
                                              std::fabs(widest->maxDepth - widest->minDepth)))
                    widest = &ranges[i];
        return widest != nullptr ? *widest : DepthRange{ 0.0f, 1.0f, 0 };
    }

    // The camera behind a D3D perspective position matrix (projection x view x model; clip z = c x view z + e, clip w =
    // view z): its near and far planes and vertical field of view, for FSR's depth tests, and whether its depth is
    // reversed (1 at the near plane, 0 at the far one). False for a matrix that is not such a projection (orthographic,
    // a HUD's, a degenerate one).
    struct Camera
    {
        float nearZ, farZ, fovY;
        bool reversed;
    };
    inline bool CameraOf(const Matrix& m, Camera& camera)
    {
        const double* r1 = &m[4];
        const double* r2 = &m[8];
        const double* r3 = &m[12];
        const double w2 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
        if (w2 < 1e-12)
            return false;
        // The w row is the view's z axis (times the model's scale); the z row is c times it, plus e in its translation.
        const double c = (r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2]) / w2;
        const double e = r2[3] - c * r3[3];
        const double cotHalf = std::sqrt((r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]) / w2);
        if (std::fabs(c) < 1e-9 || std::fabs(1.0 - c) < 1e-9 || !(cotHalf > 0.0))
            return false;
        // Depth 0 at the near plane and 1 at the far one, or the reverse.
        double nearZ = -e / c, farZ = e / (1.0 - c);
        bool reversed = false;
        if (!(nearZ > 0.0) || !(farZ > nearZ))
        {
            nearZ = e / (1.0 - c), farZ = -e / c, reversed = true;
            if (!(nearZ > 0.0) || !(farZ > nearZ))
                return false;
        }
        camera = { float(nearZ), float(farZ), float(2.0 * std::atan(1.0 / cotHalf)), reversed };
        // A game camera: planes in a plausible range (a HUD's or a sky's own matrix gives tiny or huge ones).
        return camera.fovY > 0.17f && camera.fovY < 2.5f && nearZ >= 1e-3 && nearZ <= 100.0 && farZ >= 10.0 * nearZ;
    }

    inline Matrix Multiply(const Matrix& a, const Matrix& b)
    {
        Matrix m{};
        for (uint32_t i = 0; i < 4; i++)
            for (uint32_t j = 0; j < 4; j++)
                for (uint32_t k = 0; k < 4; k++)
                    m[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        return m;
    }

    // Gauss-Jordan with partial pivoting; false when singular.
    inline bool Invert(const Matrix& in, Matrix& out)
    {
        Matrix a = in;
        out = Identity;
        for (uint32_t column = 0; column < 4; column++)
        {
            uint32_t pivot = column;
            for (uint32_t row = column + 1; row < 4; row++)
                if (std::fabs(a[row * 4 + column]) > std::fabs(a[pivot * 4 + column]))
                    pivot = row;
            if (std::fabs(a[pivot * 4 + column]) < 1e-12)
                return false;
            for (uint32_t k = 0; k < 4; k++)
            {
                std::swap(a[column * 4 + k], a[pivot * 4 + k]);
                std::swap(out[column * 4 + k], out[pivot * 4 + k]);
            }
            const double scale = 1.0 / a[column * 4 + column];
            for (uint32_t k = 0; k < 4; k++)
            {
                a[column * 4 + k] *= scale;
                out[column * 4 + k] *= scale;
            }
            for (uint32_t row = 0; row < 4; row++)
            {
                if (row == column)
                    continue;
                const double factor = a[row * 4 + column];
                for (uint32_t k = 0; k < 4; k++)
                {
                    a[row * 4 + k] -= factor * a[column * 4 + k];
                    out[row * 4 + k] -= factor * out[column * 4 + k];
                }
            }
        }
        return true;
    }

    // How far apart two matrices are, relative to their size: the largest entry difference over the largest entry.
    inline double Distance(const Matrix& a, const Matrix& b)
    {
        double difference = 0.0, size = 1e-9;
        for (uint32_t i = 0; i < 16; i++)
        {
            difference = std::max(difference, std::fabs(a[i] - b[i]));
            size = std::max({ size, std::fabs(a[i]), std::fabs(b[i]) });
        }
        return difference / size;
    }

    // The camera's motion between two frames, from the draws themselves. A draw seen in both frames (same identity:
    // shaders, index buffer, index range; the n-th draw of an identity pairs with the n-th) gives previous x
    // inverse(current): how its object's clip space moved. Every static object gives the same matrix, the camera's; a
    // moving object (a character) gives its own. The matrix that most of the drawn indices agree on wins.
    // Fixed capacity: no allocation once constructed.
    class CameraVote
    {
    public:
        static constexpr size_t MaxDraws = 4096;
        static constexpr size_t MaxCandidates = 64;
        static constexpr double Tolerance = 2e-3;

        CameraVote()
        {
            m_current.reserve(MaxDraws);
            m_previous.reserve(MaxDraws);
            m_pairs.reserve(MaxDraws);
        }

        void Add(uint64_t identity, const Matrix& position, uint32_t weight)
        {
            if (m_current.size() < MaxDraws)
                m_current.push_back({ identity, uint32_t(m_current.size()), weight, position });
        }

        // Ends the frame's scene: the current frame becomes the previous one. True, with `reprojection` (this frame's clip
        // space to the previous frame's), when one matrix has more than `quorum` of the weight of the heaviest pairs.
        bool Close(Matrix& reprojection, double quorum = 0.3)
        {
            auto order = [](const Draw& a, const Draw& b) {
                return a.identity != b.identity ? a.identity < b.identity : a.sequence < b.sequence;
            };
            std::sort(m_current.begin(), m_current.end(), order);
            m_pairs.clear();
            size_t p = 0, c = 0;
            while (p < m_previous.size() && c < m_current.size())
            {
                if (m_previous[p].identity < m_current[c].identity)
                    p++;
                else if (m_current[c].identity < m_previous[p].identity)
                    c++;
                else
                {
                    Matrix inverse;
                    if (Invert(m_current[c].position, inverse))
                        m_pairs.push_back({ double(std::min(m_previous[p].weight, m_current[c].weight)),
                            Multiply(m_previous[p].position, inverse) });
                    p++;
                    c++;
                }
            }
            std::swap(m_previous, m_current);
            m_current.clear();

            const size_t count = std::min(m_pairs.size(), MaxCandidates);
            std::partial_sort(m_pairs.begin(), m_pairs.begin() + count, m_pairs.end(),
                [](const Pair& a, const Pair& b) { return a.weight > b.weight; });
            double total = 0.0, best = 0.0;
            size_t winner = count;
            for (size_t i = 0; i < count; i++)
            {
                total += m_pairs[i].weight;
                double score = 0.0;
                for (size_t j = 0; j < count; j++)
                    if (Distance(m_pairs[i].reprojection, m_pairs[j].reprojection) < Tolerance)
                        score += m_pairs[j].weight;
                if (score > best)
                    best = score, winner = i;
            }
            // The other motions in the scene (CameraVote::Others): the heaviest pairs' clusters apart from the winner's,
            // each with a tenth of the weight at least.
            m_otherCount = 0;
            if (winner < count)
            {
                std::vector<bool> used(count, false);
                for (size_t j = 0; j < count; j++)
                    used[j] = Distance(m_pairs[winner].reprojection, m_pairs[j].reprojection) < Tolerance;
                for (size_t i = 0; i < count && m_otherCount < m_others.size(); i++)
                {
                    if (used[i])
                        continue;
                    double weight = 0.0;
                    for (size_t j = 0; j < count; j++)
                        if (!used[j] && Distance(m_pairs[i].reprojection, m_pairs[j].reprojection) < Tolerance)
                            used[j] = true, weight += m_pairs[j].weight;
                    if (weight >= 0.1 * total)
                        m_others[m_otherCount++] = m_pairs[i].reprojection;
                }
            }
            if (winner == count || best <= quorum * total)
                return false;
            reprojection = m_pairs[winner].reprojection;
            return true;
        }

        // After Close: up to two other reprojections that a tenth of the scene's weight each agreed on (draws moving
        // apart from the camera's motion: a ship the camera rides, the world passing by).
        uint32_t Others(Matrix out[2]) const
        {
            for (uint32_t i = 0; i < m_otherCount; i++)
                out[i] = m_others[i];
            return m_otherCount;
        }

        // A frame without a scene: nothing to pair the next one with.
        void Forget()
        {
            m_otherCount = 0;
            m_previous.clear();
            m_current.clear();
        }

    private:
        struct Draw
        {
            uint64_t identity;
            uint32_t sequence, weight;
            Matrix position;
        };
        struct Pair
        {
            double weight;
            Matrix reprojection;
        };
        std::vector<Draw> m_current, m_previous;
        std::vector<Pair> m_pairs;
        std::array<Matrix, 2> m_others{};
        uint32_t m_otherCount = 0;
    };
}
