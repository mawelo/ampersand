// io_meter.cpp — siehe include/qc/io_meter.hpp
#include "qc/io_meter.hpp"

#include <ProductionAutomation.pb.h>

#include <algorithm>
#include <cmath>

namespace qc::iometer {

namespace {

using Msg = cortex_protobuf_v2::IOMeterMessage;
using Get = float (*)(const Msg&);
using Has = bool (*)(const Msg&);

struct Access {
    Has has;
    Get get;
};

// Ein Eintrag je Feld. Die Lambdas sind ohne Bindung und damit gewoehnliche
// Funktionszeiger; so bleibt die Tabelle frei von Reflection.
#define QC_IOM(f) Access{[](const Msg& m) { return m.has_##f(); }, [](const Msg& m) { return m.f(); }}
constexpr std::array<Access, kChannelCount> kAccess{
    QC_IOM(input_1),       QC_IOM(input_2),       QC_IOM(return_1),      QC_IOM(return_2),
    QC_IOM(xlr_1),         QC_IOM(xlr_1_limiter), QC_IOM(xlr_2),         QC_IOM(xlr_2_limiter),
    QC_IOM(out_3),         QC_IOM(out_3_limiter), QC_IOM(out_4),         QC_IOM(out_4_limiter),
    QC_IOM(send_1),        QC_IOM(send_2),        QC_IOM(hp_l),          QC_IOM(hp_r),
    QC_IOM(usb_output_1l), QC_IOM(usb_output_1r), QC_IOM(usb_output_2l), QC_IOM(usb_output_2r),
    QC_IOM(usb_output_3l), QC_IOM(usb_output_3r), QC_IOM(usb_output_4l), QC_IOM(usb_output_4r),
    QC_IOM(usb_input_1l),  QC_IOM(usb_input_1r),  QC_IOM(usb_input_2l),  QC_IOM(usb_input_2r),
    QC_IOM(usb_input_3l),  QC_IOM(usb_input_3r),  QC_IOM(usb_input_4l),  QC_IOM(usb_input_4r),
    QC_IOM(grid_xlr_1),    QC_IOM(grid_xlr_2),    QC_IOM(grid_out_3),    QC_IOM(grid_out_4),
    QC_IOM(grid_send_1),   QC_IOM(grid_send_2),
};
#undef QC_IOM
static_assert(kAccess.size() == kChannelCount);

using enum Group;
constexpr std::array<Channel, kChannelCount> kChannels{{
    {0, 3, "Input 1", "in1", Input},          {1, 4, "Input 2", "in2", Input},
    {2, 5, "Return 1", "ret1", Input},        {3, 6, "Return 2", "ret2", Input},
    {4, 7, "XLR 1", "xlr1", Output},          {5, 8, "XLR 1 limiter", "xlr1lim", Limiter},
    {6, 9, "XLR 2", "xlr2", Output},          {7, 10, "XLR 2 limiter", "xlr2lim", Limiter},
    {8, 11, "Out 3", "out3", Output},         {9, 12, "Out 3 limiter", "out3lim", Limiter},
    {10, 13, "Out 4", "out4", Output},        {11, 14, "Out 4 limiter", "out4lim", Limiter},
    {12, 15, "Send 1", "send1", Output},      {13, 16, "Send 2", "send2", Output},
    {14, 17, "Headphones L", "hpl", Output},  {15, 18, "Headphones R", "hpr", Output},
    {16, 20, "USB out 1 L", "usbo1l", UsbOutput}, {17, 21, "USB out 1 R", "usbo1r", UsbOutput},
    {18, 22, "USB out 2 L", "usbo2l", UsbOutput}, {19, 23, "USB out 2 R", "usbo2r", UsbOutput},
    {20, 24, "USB out 3 L", "usbo3l", UsbOutput}, {21, 25, "USB out 3 R", "usbo3r", UsbOutput},
    {22, 26, "USB out 4 L", "usbo4l", UsbOutput}, {23, 27, "USB out 4 R", "usbo4r", UsbOutput},
    {24, 28, "USB in 1 L", "usbi1l", UsbInput},   {25, 29, "USB in 1 R", "usbi1r", UsbInput},
    {26, 30, "USB in 2 L", "usbi2l", UsbInput},   {27, 31, "USB in 2 R", "usbi2r", UsbInput},
    {28, 32, "USB in 3 L", "usbi3l", UsbInput},   {29, 33, "USB in 3 R", "usbi3r", UsbInput},
    {30, 34, "USB in 4 L", "usbi4l", UsbInput},   {31, 35, "USB in 4 R", "usbi4r", UsbInput},
    {32, 36, "Grid XLR 1", "gxlr1", Grid},    {33, 37, "Grid XLR 2", "gxlr2", Grid},
    {34, 38, "Grid Out 3", "gout3", Grid},    {35, 39, "Grid Out 4", "gout4", Grid},
    {36, 40, "Grid Send 1", "gsend1", Grid},  {37, 41, "Grid Send 2", "gsend2", Grid},
}};

}  // namespace

std::span<const Channel> channels() noexcept { return kChannels; }

void merge(Levels& levels, const Msg& msg) {
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        if (kAccess[i].has(msg)) levels.values[i] = kAccess[i].get(msg);
    }
    if (msg.has_hp_limiter_active()) levels.hp_limiter_active = msg.hp_limiter_active();
    ++levels.updates;
}

void Accumulator::add(const Levels& levels) noexcept {
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        auto& st = stats_[i];
        const float v = levels.values[i];
        st.last = v;
        st.max = std::max(st.max, v);
        st.sum += static_cast<double>(v);
    }
    if (levels.hp_limiter_active) ++hp_limiter_true_;
    ++messages_;
}

double to_db(float value) noexcept {
    return value > 0.0f ? 20.0 * std::log10(static_cast<double>(value)) : kFloorDb;
}

}  // namespace qc::iometer
