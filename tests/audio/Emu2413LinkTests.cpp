#include <emu2413.h>

#include <memory>

int main() {
    std::unique_ptr<OPLL, decltype(&OPLL_delete)> chip(OPLL_new(3579545, 48000), OPLL_delete);
    if (!chip) return 1;
    OPLL_setChipType(chip.get(), 0);
    OPLL_resetPatch(chip.get(), OPLL_2413_TONE);
    OPLL_writeIO(chip.get(), 0, 0x30);
    OPLL_writeIO(chip.get(), 1, 0x10);
    for (unsigned i = 0; i < 100; ++i) OPLL_calc(chip.get());
    return 0;
}
