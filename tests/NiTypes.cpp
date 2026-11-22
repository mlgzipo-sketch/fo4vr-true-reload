// The few NiPoint3 / NiMatrix3 members the tested code uses, as CommonLibF4 defines them
// (CommonLibF4/src/RE/NetImmerse/NiPoint.cpp and NiMatrix3.cpp, MIT license). The library's own files also hold
// functions that call into the game, so the unit test build cannot compile them.
#include "MathTypes.h"

namespace RE
{
    NiPoint3::NiPoint3(float a_x, float a_y, float a_z) noexcept :
        x(a_x), y(a_y), z(a_z)
    {}

    NiPoint3 NiPoint3::operator+(const NiPoint3& a_rhs) const noexcept { return NiPoint3(x + a_rhs.x, y + a_rhs.y, z + a_rhs.z); }
    NiPoint3 NiPoint3::operator-(const NiPoint3& a_rhs) const noexcept { return NiPoint3(x - a_rhs.x, y - a_rhs.y, z - a_rhs.z); }
    NiPoint3 NiPoint3::operator*(float a_scalar) const noexcept { return NiPoint3(x * a_scalar, y * a_scalar, z * a_scalar); }

    float&       NiPoint4::operator[](std::size_t a_pos) noexcept { return std::addressof(x)[a_pos]; }
    const float& NiPoint4::operator[](std::size_t a_pos) const noexcept { return std::addressof(x)[a_pos]; }

    NiMatrix3 NiMatrix3::operator*(const NiMatrix3& a_rhs) const noexcept
    {
        NiMatrix3 result;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                result.entry[r][c] = entry[r][0] * a_rhs.entry[0][c] + entry[r][1] * a_rhs.entry[1][c] + entry[r][2] * a_rhs.entry[2][c];
            }
        }
        return result;
    }

    NiPoint3 NiMatrix3::operator*(const NiPoint3& a_rhs) const noexcept
    {
        return NiPoint3(
            entry[0][0] * a_rhs.x + entry[0][1] * a_rhs.y + entry[0][2] * a_rhs.z,
            entry[1][0] * a_rhs.x + entry[1][1] * a_rhs.y + entry[1][2] * a_rhs.z,
            entry[2][0] * a_rhs.x + entry[2][1] * a_rhs.y + entry[2][2] * a_rhs.z);
    }

    void NiMatrix3::MakeIdentity() noexcept
    {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                entry[r][c] = r == c ? 1.0F : 0.0F;
            }
        }
    }

    NiMatrix3 NiMatrix3::Transpose() const noexcept
    {
        NiMatrix3 result;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                result.entry[r][c] = entry[c][r];
            }
            result.entry[r][3] = entry[r][3];
        }
        return result;
    }
}
