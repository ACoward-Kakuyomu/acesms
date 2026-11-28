#include "core/cpu/Z80.h"

#include <array>
#include <bit>
#include <utility>

namespace smsgg {

namespace {
bool indexedMemoryOpcode(uint8_t opcode) {
    return ((opcode & 0xc0) == 0x40 && opcode != 0x76
        && ((opcode & 7) == 6 || ((opcode >> 3) & 7) == 6))
        || opcode == 0x34 || opcode == 0x35 || opcode == 0x36 || (opcode & 0xc7) == 0x86;
}

// Size after any leading DD/FD bytes; ED's second opcode selects its final size.
size_t instructionSize(uint8_t opcode, uint8_t second, bool index) {
    if (opcode == 0xed) return (second & 0xc7) == 0x43 ? 4 : 2;
    if (opcode == 0xcb) return index ? 3 : 2;
    if ((opcode & 0xcf) == 0x01 || opcode == 0x22 || opcode == 0x2a
        || opcode == 0x32 || opcode == 0x3a || opcode == 0xc3 || opcode == 0xcd
        || (opcode & 0xc7) == 0xc2 || (opcode & 0xc7) == 0xc4) return 3;
    const size_t length = (opcode & 0xc7) == 0x06 || (opcode & 0xc7) == 0xc6
        || opcode == 0x10 || opcode == 0x18 || (opcode & 0xe7) == 0x20
        || opcode == 0xd3 || opcode == 0xdb ? 2 : 1;
    return length + (index && indexedMemoryOpcode(opcode) ? 1 : 0);
}

uint16_t readWord(Z80Memory& memory, uint16_t address) {
    const uint8_t low = memory.read8(address);
    const uint8_t high = memory.read8(static_cast<uint16_t>(address + 1));
    return static_cast<uint16_t>(low | (high << 8));
}

void writeWord(Z80Memory& memory, uint16_t address, uint16_t value) {
    memory.write8(address, static_cast<uint8_t>(value));
    memory.write8(static_cast<uint16_t>(address + 1), static_cast<uint8_t>(value >> 8));
}
} // namespace

Z80::Z80(Z80Registers registers) : registers_(registers) {}

const Z80Registers& Z80::registers() const noexcept {
    return registers_;
}

Z80State Z80::captureState() const noexcept {
    return {registers_,iff1_,iff2_,halted_,flagsModified_,ldAir_,eiDelay_,interruptMode_,memptr_};
}

void Z80::restoreState(const Z80State& state) noexcept {
    registers_=state.registers;
    iff1_=state.iff1;iff2_=state.iff2;halted_=state.halted;
    flagsModified_=state.flagsModified;ldAir_=state.ldAir;
    eiDelay_=state.eiDelay;interruptMode_=state.interruptMode;memptr_=state.memptr;
}

void Z80::reset() {
    registers_.pc = 0;
    registers_.i = 0;
    registers_.r = 0;
    iff1_ = false;
    iff2_ = false;
    halted_ = false;
    flagsModified_ = false;
    ldAir_ = false;
    eiDelay_ = 0;
    interruptMode_ = 0;
    memptr_ = 0;
}

uint32_t Z80::acceptNmi(Z80Memory& memory) {
    pushWord(memory, registers_.pc);
    registers_.pc = memptr_ = 0x66;
    registers_.r = static_cast<uint8_t>((registers_.r & 0x80) | ((registers_.r + 1) & 0x7f));
    iff1_ = false;
    halted_ = false;
    flagsModified_ = false;
    ldAir_ = false;
    return 11;
}

std::optional<uint32_t> Z80::acceptInterrupt(std::span<const uint8_t> bytes, Z80Memory* memory, Z80Io* io,
    uint64_t startTstate) {
    if (!canAcceptMaskableInterrupt()) return 0;
    if (interruptMode_ == 0) {
        constexpr uint32_t acknowledgeExtraTstates = 2;
        Z80 accepted = *this;
        accepted.iff1_ = accepted.iff2_ = accepted.halted_ = accepted.flagsModified_ = false;
        if (accepted.ldAir_) accepted.registers_.setF(static_cast<uint8_t>(accepted.registers_.f() & 0xfb));
        --accepted.registers_.pc; // The injected opcode itself does not consume a memory byte.
        const auto elapsed = accepted.executeInstruction(bytes, memory, io, startTstate + acknowledgeExtraTstates);
        if (!elapsed) return std::nullopt;
        *this = accepted;
        return *elapsed + acknowledgeExtraTstates;
    }
    if (!memory || (interruptMode_ == 2 && bytes.empty())) return std::nullopt;
    pushWord(*memory, registers_.pc);
    registers_.pc = interruptMode_ == 1 ? 0x38
        : readWord(*memory, static_cast<uint16_t>((registers_.i << 8) | bytes[0]));
    memptr_ = registers_.pc;
    registers_.r = static_cast<uint8_t>((registers_.r & 0x80) | ((registers_.r + 1) & 0x7f));
    iff1_ = iff2_ = false;
    halted_ = false;
    flagsModified_ = false;
    if (ldAir_) registers_.setF(static_cast<uint8_t>(registers_.f() & 0xfb));
    ldAir_ = false;
    return interruptMode_ == 1 ? 13 : 19;
}

void Z80::pushWord(Z80Memory& memory, uint16_t value) {
    memory.write8(--registers_.sp, static_cast<uint8_t>(value >> 8));
    memory.write8(--registers_.sp, static_cast<uint8_t>(value));
}

uint16_t Z80::popWord(Z80Memory& memory) {
    const uint16_t value = readWord(memory, registers_.sp);
    registers_.sp = static_cast<uint16_t>(registers_.sp + 2);
    return value;
}

uint16_t& Z80::registerPair(uint8_t code) {
    switch (code) {
    case 0: return registers_.bc;
    case 1: return registers_.de;
    case 2: return registers_.hl;
    default: return registers_.sp; // Two-bit pair code 3.
    }
}

// Callers decode register codes first and exclude 6, which denotes (HL).
uint8_t Z80::readRegister8(uint8_t code) const {
    switch (code) {
    case 0: return registers_.b();
    case 1: return registers_.c();
    case 2: return registers_.d();
    case 3: return registers_.e();
    case 4: return registers_.h();
    case 5: return registers_.l();
    default: return registers_.a(); // Only code 7 remains.
    }
}

void Z80::writeRegister8(uint8_t code, uint8_t value) {
    switch (code) {
    case 0: registers_.setB(value); break;
    case 1: registers_.setC(value); break;
    case 2: registers_.setD(value); break;
    case 3: registers_.setE(value); break;
    case 4: registers_.setH(value); break;
    case 5: registers_.setL(value); break;
    default: registers_.setA(value); break; // Only code 7 remains; caller excludes 6.
    }
}

uint8_t Z80::cb8(uint8_t opcode, uint8_t before, uint8_t xy) {
    const uint8_t incoming = registers_.f() & 1;
    const auto operation = (opcode >> 3) & 7;
    if (opcode >= 0x40) {
        const uint8_t mask = static_cast<uint8_t>(1u << operation);
        if (opcode < 0x80) {
            const bool set = (before & mask) != 0;
            registers_.setF(static_cast<uint8_t>(incoming | 0x10 | (xy & 0x28)
                | (!set ? 0x44 : 0) | (operation == 7 && set ? 0x80 : 0)));
            return before;
        }
        return static_cast<uint8_t>(opcode < 0xc0 ? before & ~mask : before | mask);
    }
    const uint8_t carry = (operation & 1) != 0 ? before & 1 : before >> 7;
    uint8_t result;
    switch (operation) {
    case 0: result = static_cast<uint8_t>((before << 1) | carry); break;
    case 1: result = static_cast<uint8_t>((before >> 1) | (carry << 7)); break;
    case 2: result = static_cast<uint8_t>((before << 1) | incoming); break;
    case 3: result = static_cast<uint8_t>((before >> 1) | (incoming << 7)); break;
    case 4: result = static_cast<uint8_t>(before << 1); break;
    case 5: result = static_cast<uint8_t>((before >> 1) | (before & 0x80)); break;
    case 6: result = static_cast<uint8_t>((before << 1) | 1); break;
    default: result = before >> 1; break; // SRL.
    }
    registers_.setF(static_cast<uint8_t>(
        (result & 0xa8) | (result == 0 ? 0x40 : 0)
        | (std::popcount(static_cast<unsigned>(result)) % 2 == 0 ? 0x04 : 0) | carry));
    return result;
}

void Z80::alu8(uint8_t operation, uint8_t operand) {
    const uint8_t a = registers_.a();
    if (operation >= 4 && operation <= 6) {
        const uint8_t result = operation == 4 ? a & operand
            : operation == 5 ? a ^ operand : a | operand;
        registers_.setA(result);
        registers_.setF(static_cast<uint8_t>(
            (result & 0xa8) | (result == 0 ? 0x40 : 0) | (operation == 4 ? 0x10 : 0)
            | (std::popcount(static_cast<unsigned>(result)) % 2 == 0 ? 0x04 : 0)));
        return;
    }
    const bool subtract = (operation & 2) != 0;
    const int carry = operation == 1 || operation == 3 ? registers_.f() & 1 : 0;
    const int wide = subtract ? a - operand - carry : a + operand + carry;
    const uint8_t result = static_cast<uint8_t>(wide);
    const auto overflow = subtract
        ? (a ^ operand) & (a ^ result) : ~(a ^ operand) & (a ^ result);
    if (operation != 7) {
        registers_.setA(result);
    }
    const uint8_t xy = operation == 7 ? operand : result;
    registers_.setF(static_cast<uint8_t>(
        (result & 0x80) | (xy & 0x28) | (result == 0 ? 0x40 : 0)
        | ((a ^ operand ^ result) & 0x10) | ((overflow & 0x80) >> 5)
        | (subtract ? 0x02 : 0) | (wide < 0 || wide > 0xff ? 1 : 0)));
}

void Z80::executeAccumulator(uint8_t opcode) {
    const uint8_t a = registers_.a();
    const uint8_t flags = registers_.f();
    if (opcode < 0x20) {
        const bool right = (opcode & 0x08) != 0;
        const bool throughCarry = (opcode & 0x10) != 0;
        const uint8_t carry = right ? a & 1 : a >> 7;
        const uint8_t incoming = throughCarry ? flags & 1 : carry;
        const uint8_t result = static_cast<uint8_t>(
            right ? (a >> 1) | (incoming << 7) : (a << 1) | incoming);
        registers_.setA(result);
        registers_.setF(static_cast<uint8_t>((flags & 0xc4) | (result & 0x28) | carry));
    } else if (opcode == 0x27) {
        const bool carry = (flags & 1) != 0 || a > 0x99;
        const uint8_t correction = static_cast<uint8_t>(
            (((flags & 0x10) != 0 || (a & 0x0f) > 9) ? 0x06 : 0) | (carry ? 0x60 : 0));
        const uint8_t result = static_cast<uint8_t>(
            (flags & 2) != 0 ? a - correction : a + correction);
        registers_.setA(result);
        registers_.setF(static_cast<uint8_t>(
            (flags & 2) | (carry ? 1 : 0) | (result & 0xa8) | (result == 0 ? 0x40 : 0)
            | ((a ^ result) & 0x10)
            | (std::popcount(static_cast<unsigned>(result)) % 2 == 0 ? 0x04 : 0)));
    } else if (opcode == 0x2f) {
        const uint8_t result = static_cast<uint8_t>(~a);
        registers_.setA(result);
        registers_.setF(static_cast<uint8_t>((flags & 0xc5) | (result & 0x28) | 0x12));
    } else if (opcode == 0x37) {
        const uint8_t xy = static_cast<uint8_t>(a | (flagsModified_ ? 0 : flags));
        registers_.setF(static_cast<uint8_t>((flags & 0xc4) | (xy & 0x28) | 1));
    } else { // CCF (3F).
        const uint8_t xy = static_cast<uint8_t>(a | (flagsModified_ ? 0 : flags));
        registers_.setF(static_cast<uint8_t>(
            (flags & 0xc4) | (xy & 0x28) | ((flags & 1) != 0 ? 0x10 : 1)));
    }
}

uint32_t Z80::executeBlock(uint8_t opcode, Z80Memory& memory, Z80Io* io, uint64_t startTstate) {
    const int direction = (opcode & 8) != 0 ? -1 : 1;
    if ((opcode & 2) != 0) {
        constexpr uint32_t transferTstates = 16;
        const bool output = (opcode & 1) != 0;
        const uint16_t oldPort = registers_.bc;
        // Input reads the port before the final three-T-state memory write.
        const uint8_t data = output ? memory.read8(registers_.hl) : io->readPort(oldPort, startTstate + 13);
        registers_.setB(static_cast<uint8_t>(registers_.b() - 1));
        const uint16_t port = output ? registers_.bc : oldPort;
        // The repeat adds five T-states after the write cycle.
        if (output) io->writePort(port, data, startTstate + transferTstates);
        else memory.write8(registers_.hl, data);
        registers_.hl = static_cast<uint16_t>(registers_.hl + direction);
        memptr_ = static_cast<uint16_t>(port + direction);
        const unsigned sum = data + (output ? registers_.l() : static_cast<uint8_t>(registers_.c() + direction));
        const uint8_t b = registers_.b();
        uint8_t flags = static_cast<uint8_t>((b & 0xa8) | (b == 0 ? 0x40 : 0)
            | (sum > 255 ? 0x11 : 0) | ((data & 0x80) != 0 ? 2 : 0)
            | (std::popcount((sum & 7) ^ b) % 2 == 0 ? 4 : 0));
        if ((opcode & 0x10) != 0 && b != 0) {
            unsigned parityOperand = b;
            if (sum > 255) {
                const bool negative = (data & 0x80) != 0;
                parityOperand = static_cast<uint8_t>(b + (negative ? -1 : 1));
                flags = static_cast<uint8_t>((flags & 0xef) | ((b & 15) == (negative ? 0 : 15) ? 0x10 : 0));
            }
            if (std::popcount(parityOperand & 7) % 2 != 0) flags ^= 4;
            registers_.setF(static_cast<uint8_t>((flags & 0xd7) | ((registers_.pc >> 8) & 0x28)));
            registers_.pc = static_cast<uint16_t>(registers_.pc - 2);
            return transferTstates + 5;
        }
        registers_.setF(flags);
        return transferTstates;
    }
    const uint8_t value = memory.read8(registers_.hl);
    const bool compare = (opcode & 1) != 0;
    if (!compare) {
        memory.write8(registers_.de, value);
        registers_.de = static_cast<uint16_t>(registers_.de + direction);
    }
    registers_.hl = static_cast<uint16_t>(registers_.hl + direction);
    --registers_.bc;
    if (compare) {
        const uint8_t result = static_cast<uint8_t>(registers_.a() - value);
        const bool half = (registers_.a() & 15) < (value & 15);
        const uint8_t adjusted = static_cast<uint8_t>(result - (half ? 1 : 0));
        registers_.setF(static_cast<uint8_t>((registers_.f() & 1) | (result & 0x80)
            | (result == 0 ? 0x40 : 0) | (half ? 0x10 : 0) | (adjusted & 8)
            | ((adjusted & 2) << 4) | (registers_.bc != 0 ? 4 : 0) | 2));
        memptr_ = static_cast<uint16_t>(memptr_ + direction);
    } else {
        const uint8_t sum = static_cast<uint8_t>(registers_.a() + value);
        registers_.setF(static_cast<uint8_t>((registers_.f() & 0xc1) | (sum & 8)
            | ((sum & 2) << 4) | (registers_.bc != 0 ? 4 : 0)));
    }
    if ((opcode & 0x10) != 0 && registers_.bc != 0 && (!compare || (registers_.f() & 0x40) == 0)) {
        memptr_ = static_cast<uint16_t>(registers_.pc + 1);
        registers_.setF(static_cast<uint8_t>((registers_.f() & 0xd7) | ((registers_.pc >> 8) & 0x28)));
        // The common instruction tail advances past ED/opcode after this rewind.
        registers_.pc = static_cast<uint16_t>(registers_.pc - 2);
        return 21;
    }
    return 16;
}

std::optional<uint32_t> Z80::executeEd(uint8_t opcode, Z80Memory* memory, Z80Io* io, uint64_t startTstate) {
    const bool block = (opcode & 0xe4) == 0xa0;
    if (opcode == 0x77 || opcode == 0x7f || ((opcode < 0x40 || opcode >= 0x80) && !block)) {
        return 8; // Unassigned ED sub-opcodes are two-byte NOPs.
    }
    if (block) {
        if (!memory || ((opcode & 2) != 0 && !io)) return std::nullopt;
        return executeBlock(opcode, *memory, io, startTstate);
    }
    if ((opcode & 0xc6) == 0x40) {
        if (!io) return std::nullopt;
        constexpr uint32_t elapsed = 12;
        const uint16_t port = registers_.bc;
        const uint8_t code = (opcode >> 3) & 7;
        if ((opcode & 1) != 0) {
            io->writePort(port, code == 6 ? 0 : readRegister8(code), startTstate + elapsed);
        } else {
            const uint8_t value = io->readPort(port, startTstate + elapsed);
            if (code != 6) writeRegister8(code, value);
            registers_.setF(static_cast<uint8_t>((registers_.f() & 1) | (value & 0xa8)
                | (value == 0 ? 0x40 : 0) | (std::popcount(static_cast<unsigned>(value)) % 2 == 0 ? 4 : 0)));
        }
        memptr_ = static_cast<uint16_t>(port + 1);
        return elapsed;
    }
    if (opcode == 0x67 || opcode == 0x6f) {
        if (!memory) return std::nullopt;
        const uint8_t a = registers_.a();
        const uint8_t before = memory->read8(registers_.hl);
        const bool left = opcode == 0x6f;
        const uint8_t result = static_cast<uint8_t>((a & 0xf0) | (left ? before >> 4 : before & 0x0f));
        const uint8_t data = static_cast<uint8_t>(left ? (before << 4) | (a & 0x0f) : (a << 4) | (before >> 4));
        memory->write8(registers_.hl, data);
        memptr_ = static_cast<uint16_t>(registers_.hl + 1);
        registers_.setA(result);
        registers_.setF(static_cast<uint8_t>((registers_.f() & 1) | (result & 0xa8)
            | (result == 0 ? 0x40 : 0) | (std::popcount(static_cast<unsigned>(result)) % 2 == 0 ? 4 : 0)));
        return 18;
    }
    if ((opcode & 0xc7) == 0x46) {
        const auto mode = (opcode >> 3) & 3;
        interruptMode_ = static_cast<uint8_t>(mode < 2 ? 0 : mode - 1);
        return 8;
    }
    if (opcode == 0x47 || opcode == 0x4f) {
        if (opcode == 0x47) registers_.i = registers_.a();
        else registers_.r = registers_.a();
        return 9;
    }
    if (opcode == 0x57 || opcode == 0x5f) {
        // ED and its sub-opcode have already been fetched when R is read.
        const uint8_t value = opcode == 0x57 ? registers_.i : static_cast<uint8_t>(
            (registers_.r & 0x80) | ((registers_.r + 2) & 0x7f));
        registers_.setA(value);
        registers_.setF(static_cast<uint8_t>((registers_.f() & 1) | (value & 0xa8)
            | (value == 0 ? 0x40 : 0) | (iff2_ ? 4 : 0)));
        return 9;
    }
    if ((opcode & 0xc7) == 0x44) {
        const uint8_t operand = registers_.a();
        registers_.setA(0);
        alu8(2, operand);
        return 8;
    }
    if ((opcode & 0xc7) == 0x42) {
        const uint16_t before = registers_.hl;
        const uint16_t operand = registerPair((opcode >> 4) & 3);
        memptr_ = static_cast<uint16_t>(before + 1);
        const int carry = registers_.f() & 1;
        const bool subtract = (opcode & 8) == 0;
        const int wide = subtract ? before - operand - carry : before + operand + carry;
        const uint16_t result = static_cast<uint16_t>(wide);
        const auto overflow = subtract
            ? (before ^ operand) & (before ^ result) : ~(before ^ operand) & (before ^ result);
        registers_.hl = result;
        registers_.setF(static_cast<uint8_t>(
            ((result >> 8) & 0xa8) | (result == 0 ? 0x40 : 0)
            | (((before ^ operand ^ result) & 0x1000) >> 8) | ((overflow & 0x8000) >> 13)
            | (subtract ? 2 : 0) | (wide < 0 || wide > 0xffff ? 1 : 0)));
        return 15;
    }
    return std::nullopt;
}

bool Z80::conditionMet(uint8_t code) const {
    constexpr uint8_t masks[]{0x40, 0x01, 0x04, 0x80}; // Z, C, PV, S.
    return ((registers_.f() & masks[code >> 1]) != 0) == ((code & 1) != 0);
}

std::variant<uint32_t, CpuStepError> Z80::step(Z80Memory& memory, Z80Io& io, uint64_t startTstate) {
    if (halted_) return *executeOpcode(0x00);
    size_t prefixes = 0;
    uint8_t lastPrefix = 0;
    uint16_t address = registers_.pc;
    std::array<uint8_t, 4> body{};
    for (;;) {
        body[0] = memory.read8(address++);
        if (body[0] != 0xdd && body[0] != 0xfd) break;
        lastPrefix = body[0];
        if (++prefixes == 65536) return CpuStepError::PrefixRun;
    }
    size_t fetched = 1;
    if (body[0] == 0xed) body[fetched++] = memory.read8(address++);
    const auto size = instructionSize(body[0], body[1], prefixes != 0);
    while (fetched < size) body[fetched++] = memory.read8(address++);
    // All opcodes are supported; all required bytes and both connections are present.
    return *executePrefixed(std::span<const uint8_t>(body.data(), size), prefixes, lastPrefix, &memory, &io,
        startTstate);
}

std::optional<uint32_t> Z80::executeInstruction(std::span<const uint8_t> bytes, Z80Memory* memory, Z80Io* io,
    uint64_t startTstate) {
    if (bytes.empty()) return std::nullopt;
    if (halted_) return executeOpcode(0x00);
    size_t prefixes = 0;
    while (prefixes < bytes.size() && (bytes[prefixes] == 0xdd || bytes[prefixes] == 0xfd)) {
        ++prefixes;
    }
    if (prefixes == bytes.size()) return std::nullopt;
    return executePrefixed(bytes.subspan(prefixes), prefixes, prefixes ? bytes[prefixes - 1] : 0, memory, io,
        startTstate);
}

std::optional<uint32_t> Z80::executePrefixed(std::span<const uint8_t> instruction,
    size_t prefixes, uint8_t lastPrefix, Z80Memory* memory, Z80Io* io, uint64_t startTstate) {
    const uint8_t opcode = instruction[0];
    if (instruction.size() < instructionSize(opcode, instruction.size() > 1 ? instruction[1] : 0,
        prefixes != 0)) return std::nullopt;
    const std::optional<uint8_t> operand = instruction.size() > 1
        ? std::optional<uint8_t>{instruction[1]} : std::nullopt;
    const std::optional<uint8_t> high = instruction.size() > 2
        ? std::optional<uint8_t>{instruction[2]} : std::nullopt;
    const std::optional<uint8_t> last = instruction.size() > 3
        ? std::optional<uint8_t>{instruction[3]} : std::nullopt;
    if (prefixes == 0) return executeDecoded(opcode, operand, high, last, memory, io, startTstate);
    const bool indexedCb = opcode == 0xcb;
    const bool indexedMemory = indexedMemoryOpcode(opcode);
    const bool indexed = indexedMemory || indexedCb;

    const uint16_t oldPc = registers_.pc;
    const uint8_t oldR = registers_.r;
    registers_.pc = static_cast<uint16_t>(oldPc + prefixes);
    registers_.r = static_cast<uint8_t>((oldR & 0x80) | ((oldR + (prefixes & 0x7f)) & 0x7f));
    auto& selected = lastPrefix == 0xdd ? registers_.ix : registers_.iy;
    const std::optional<uint16_t> address = indexed ? std::optional<uint16_t>{static_cast<uint16_t>(
        selected + *operand - (*operand < 128 ? 0 : 256))} : std::nullopt;
    const bool substitute = !indexed && opcode != 0xed && opcode != 0xeb && opcode != 0xd9;
    if (substitute) std::swap(registers_.hl, selected);
    if (opcode == 0x37 || opcode == 0x3f) flagsModified_ = false;
    const auto prefixTstates = static_cast<uint32_t>(prefixes) * 4;
    const auto elapsed = executeDecoded(opcode, indexed ? high : operand,
        indexed ? std::nullopt : high, last, memory, io, startTstate + prefixTstates, address);
    if (substitute) std::swap(registers_.hl, selected);
    if (!elapsed) {
        registers_.pc = oldPc;
        registers_.r = oldR;
        return std::nullopt;
    }
    return *elapsed + prefixTstates;
}

std::optional<uint32_t> Z80::executeOpcode(
    uint8_t opcode, std::optional<uint8_t> immediate, std::optional<uint8_t> immediateHigh,
    Z80Memory* memory, Z80Io* io) {
    if (!halted_ && (opcode == 0xdd || opcode == 0xfd)) {
        if (!immediate && immediateHigh) return std::nullopt;
        const std::array<uint8_t, 3> bytes{opcode, immediate.value_or(0), immediateHigh.value_or(0)};
        return executeInstruction(std::span<const uint8_t>(bytes.data(), immediate ? (immediateHigh ? 3 : 2) : 1), memory, io);
    }
    return executeDecoded(opcode, immediate, immediateHigh, std::nullopt, memory, io, 0);
}

std::optional<uint32_t> Z80::executeDecoded(uint8_t opcode, std::optional<uint8_t> immediate,
    std::optional<uint8_t> immediateHigh, std::optional<uint8_t> immediateLast, Z80Memory* memory, Z80Io* io,
    uint64_t startTstate, std::optional<uint16_t> indexedAddress) {
    const uint16_t operandAddress = indexedAddress.value_or(registers_.hl);
    const bool immediateLoad = (opcode & 0xc7) == 0x06;
    const bool incrementOrDecrement = (opcode & 0xc6) == 0x04;
    uint16_t length = 1;
    uint32_t elapsed = 4;
    uint8_t refreshes = 1;
    bool modifiesFlags = false;
    std::optional<uint16_t> branchTarget;
    const bool registerAlu = (opcode & 0xc0) == 0x80;
    const bool immediateAlu = (opcode & 0xc7) == 0xc6;
    const bool loadPair = (opcode & 0xcf) == 0x01;
    const bool incrementPair = (opcode & 0xc7) == 0x03;
    if (halted_) {
        length = 0;
    } else if (opcode == 0x76) {
        halted_ = true;
    } else if (opcode == 0xd3 || opcode == 0xdb) {
        if (!io || !immediate) return std::nullopt;
        elapsed = 11;
        const uint8_t a = registers_.a();
        const uint16_t port = static_cast<uint16_t>((a << 8) | *immediate);
        if (opcode == 0xdb) {
            registers_.setA(io->readPort(port, startTstate + elapsed));
            memptr_ = static_cast<uint16_t>(port + 1);
        } else {
            io->writePort(port, a, startTstate + elapsed);
            memptr_ = static_cast<uint16_t>((a << 8) | ((*immediate + 1) & 0xff));
        }
        length = 2;
    } else if (opcode == 0xcd || (opcode & 0xc7) == 0xc4) {
        if (!immediate || !immediateHigh) return std::nullopt;
        const bool taken = opcode == 0xcd || conditionMet((opcode >> 3) & 7);
        if (taken && !memory) return std::nullopt;
        memptr_ = static_cast<uint16_t>(*immediate | (*immediateHigh << 8));
        if (taken) {
            pushWord(*memory, static_cast<uint16_t>(registers_.pc + 3));
            branchTarget = memptr_;
        }
        length = 3;
        elapsed = taken ? 17 : 10;
    } else if (opcode == 0xc9 || (opcode & 0xc7) == 0xc0) {
        const bool taken = opcode == 0xc9 || conditionMet((opcode >> 3) & 7);
        if (taken && !memory) return std::nullopt;
        if (taken) {
            memptr_ = popWord(*memory);
            branchTarget = memptr_;
        }
        elapsed = taken ? (opcode == 0xc9 ? 10 : 11) : 5;
    } else if ((opcode & 0xc7) == 0xc7) {
        if (!memory) return std::nullopt;
        pushWord(*memory, static_cast<uint16_t>(registers_.pc + 1));
        memptr_ = opcode & 0x38;
        branchTarget = memptr_;
        elapsed = 11;
    } else if ((opcode & 0xcf) == 0xc5 || (opcode & 0xcf) == 0xc1) {
        if (!memory) return std::nullopt;
        const auto code = (opcode >> 4) & 3;
        auto& pair = code == 3 ? registers_.af : registerPair(code);
        if ((opcode & 4) != 0) {
            pushWord(*memory, pair);
            elapsed = 11;
        } else {
            pair = popWord(*memory);
            elapsed = 10;
        }
    } else if (opcode == 0xe3) {
        if (!memory) return std::nullopt;
        const uint16_t before = registers_.hl;
        const uint16_t value = readWord(*memory, registers_.sp);
        memory->write8(static_cast<uint16_t>(registers_.sp + 1), static_cast<uint8_t>(before >> 8));
        memory->write8(registers_.sp, static_cast<uint8_t>(before));
        registers_.hl = value;
        memptr_ = value;
        elapsed = 19;
    } else if (opcode == 0x22 || opcode == 0x2a) {
        if (!memory || !immediate || !immediateHigh) return std::nullopt;
        const uint16_t address = static_cast<uint16_t>(*immediate | (*immediateHigh << 8));
        if (opcode == 0x2a) registers_.hl = readWord(*memory, address);
        else writeWord(*memory, address, registers_.hl);
        memptr_ = static_cast<uint16_t>(address + 1);
        length = 3;
        elapsed = 16;
    } else if ((opcode & 0xe7) == 0x02 || opcode == 0x32 || opcode == 0x3a) {
        const bool absolute = opcode == 0x32 || opcode == 0x3a;
        if (!memory || (absolute && (!immediate || !immediateHigh))) return std::nullopt;
        const uint16_t address = absolute ? static_cast<uint16_t>(*immediate | (*immediateHigh << 8))
            : (opcode & 0x10) != 0 ? registers_.de : registers_.bc;
        if ((opcode & 8) != 0) registers_.setA(memory->read8(address));
        else memory->write8(address, registers_.a());
        memptr_ = (opcode & 8) != 0 ? static_cast<uint16_t>(address + 1)
            : static_cast<uint16_t>((registers_.a() << 8) | ((address + 1) & 0xff));
        length = absolute ? 3 : 1;
        elapsed = absolute ? 13 : 7;
    } else if (opcode == 0xf3) {
        iff1_ = false;
        iff2_ = false;
        eiDelay_ = 0;
    } else if (opcode == 0xfb) {
        iff1_ = true;
        iff2_ = true;
        eiDelay_ = 2; // Decrement at this and the following instruction boundary.
    } else if (opcode == 0xed) {
        if (!immediate) {
            return std::nullopt;
        }
        if ((*immediate & 0xc7) == 0x45) {
            if (!memory) return std::nullopt;
            memptr_ = popWord(*memory);
            branchTarget = memptr_;
            iff1_ = iff2_;
            length = 2;
            elapsed = 14;
        } else if ((*immediate & 0xc7) == 0x43) {
            if (!memory || !immediateHigh || !immediateLast) return std::nullopt;
            const uint16_t address = static_cast<uint16_t>(*immediateHigh | (*immediateLast << 8));
            auto& pair = registerPair((*immediate >> 4) & 3);
            if ((*immediate & 8) != 0) pair = readWord(*memory, address);
            else writeWord(*memory, address, pair);
            memptr_ = static_cast<uint16_t>(address + 1);
            length = 4;
            elapsed = 20;
        } else {
            const auto result = executeEd(*immediate, memory, io, startTstate);
            if (!result) return std::nullopt;
            length = 2;
            elapsed = *result;
        }
        refreshes = *immediate == 0x4f ? 0 : 2; // LD R,A overwrites the fetched R.
        modifiesFlags = (*immediate & 0xc7) == 0x44 || (*immediate & 0xc7) == 0x42
            || (*immediate & 0xc7) == 0x40 || *immediate == 0x57 || *immediate == 0x5f
            || *immediate == 0x67 || *immediate == 0x6f || (*immediate & 0xe4) == 0xa0;
    } else if (opcode == 0xcb) {
        const bool indirect = indexedAddress || (immediate && (*immediate & 7) == 6);
        if (!immediate || (indirect && !memory)) {
            return std::nullopt;
        }
        const bool bit = (*immediate & 0xc0) == 0x40;
        const uint8_t before = indirect ? memory->read8(operandAddress) : readRegister8(*immediate & 7);
        const uint8_t xy = indirect ? static_cast<uint8_t>(indexedAddress.value_or(memptr_) >> 8) : before;
        const uint8_t result = cb8(*immediate, before, xy);
        if (!bit) {
            if (indirect) memory->write8(operandAddress, result);
            if (!indirect || (indexedAddress && (*immediate & 7) != 6)) writeRegister8(*immediate & 7, result);
        }
        modifiesFlags = *immediate < 0x80; // Rotates/shifts and BIT, but not RES/SET.
        length = 2;
        refreshes = 2;
        elapsed = indirect ? (bit ? 12 : 15) : 8;
    } else if (opcode == 0xc3 || (opcode & 0xc7) == 0xc2) {
        if (!immediate || !immediateHigh) {
            return std::nullopt;
        }
        memptr_ = static_cast<uint16_t>(*immediate | (*immediateHigh << 8));
        if (opcode == 0xc3 || conditionMet((opcode >> 3) & 7)) branchTarget = memptr_;
        length = 3;
        elapsed = 10;
    } else if (opcode == 0xe9) {
        branchTarget = registers_.hl;
    } else if (opcode == 0x10 || opcode == 0x18 || (opcode & 0xe7) == 0x20) {
        if (!immediate) {
            return std::nullopt;
        }
        bool taken;
        if (opcode == 0x10) {
            registers_.setB(static_cast<uint8_t>(registers_.b() - 1));
            taken = registers_.b() != 0;
            elapsed = taken ? 13 : 8;
        } else {
            taken = opcode == 0x18 || conditionMet((opcode >> 3) & 3);
            elapsed = taken ? 12 : 7;
        }
        if (taken) {
            const int displacement = *immediate - (*immediate < 128 ? 0 : 256);
            branchTarget = static_cast<uint16_t>(registers_.pc + 2 + displacement);
            memptr_ = *branchTarget;
        }
        length = 2;
    } else if (loadPair || incrementPair) {
        if (loadPair && (!immediate || !immediateHigh)) {
            return std::nullopt;
        }
        auto& pair = registerPair((opcode >> 4) & 3);
        if (loadPair) {
            pair = static_cast<uint16_t>(*immediate | (*immediateHigh << 8));
            length = 3;
            elapsed = 10;
        } else {
            pair = static_cast<uint16_t>(pair + ((opcode & 8) != 0 ? -1 : 1));
            elapsed = 6;
        }
    } else if ((opcode & 0xcf) == 0x09) {
        const uint16_t before = registers_.hl;
        memptr_ = static_cast<uint16_t>(before + 1);
        const uint16_t operand = registerPair((opcode >> 4) & 3);
        const uint32_t wide = static_cast<uint32_t>(before) + operand;
        registers_.hl = static_cast<uint16_t>(wide);
        registers_.setF(static_cast<uint8_t>(
            (registers_.f() & 0xc4) | ((registers_.hl >> 8) & 0x28)
            | (((before ^ operand ^ registers_.hl) & 0x1000) >> 8)
            | (wide > 0xffff ? 1 : 0)));
        elapsed = 11;
        modifiesFlags = true;
    } else if (opcode == 0x08) {
        std::swap(registers_.af, registers_.afAlternate);
    } else if (opcode == 0xd9) {
        std::swap(registers_.bc, registers_.bcAlternate);
        std::swap(registers_.de, registers_.deAlternate);
        std::swap(registers_.hl, registers_.hlAlternate);
    } else if (opcode == 0xeb) {
        std::swap(registers_.de, registers_.hl);
    } else if (opcode == 0xf9) {
        registers_.sp = registers_.hl;
        elapsed = 6;
    } else if (registerAlu || immediateAlu) {
        const bool memoryOperand = registerAlu && (opcode & 7) == 6;
        if ((memoryOperand && !memory) || (immediateAlu && !immediate)) {
            return std::nullopt;
        }
        const uint8_t operand = immediateAlu ? *immediate
            : memoryOperand ? memory->read8(operandAddress) : readRegister8(opcode & 7);
        alu8((opcode >> 3) & 7, operand);
        modifiesFlags = true;
        length = immediateAlu ? 2 : 1;
        elapsed = immediateAlu || memoryOperand ? 7 : 4;
    } else if ((opcode & 0xc0) == 0x40 || immediateLoad || incrementOrDecrement) {
        const auto destination = (opcode >> 3) & 0x07;
        const auto source = incrementOrDecrement ? destination : opcode & 0x07;
        // Register code 6 denotes (HL); immediate LD uses 110 as opcode bits.
        // HALT (0x76) is handled above.
        const bool memoryOperand = destination == 6 || (!immediateLoad && source == 6);
        if ((memoryOperand && !memory) || (immediateLoad && !immediate)) {
            return std::nullopt;
        }

        uint8_t value;
        if (immediateLoad) {
            value = *immediate;
            length = 2;
            elapsed = 7;
        } else {
            value = source == 6 ? memory->read8(operandAddress)
                : readRegister8(static_cast<uint8_t>(source));
        }
        if (memoryOperand) elapsed = incrementOrDecrement ? 11 : immediateLoad ? 10 : 7;
        if (incrementOrDecrement) {
            modifiesFlags = true;
            const bool decrement = (opcode & 0x01) != 0;
            const uint8_t before = value;
            value = static_cast<uint8_t>(before + (decrement ? -1 : 1));
            // S/5/3 come from the result; C is preserved. PV is overflow, not parity.
            registers_.setF(static_cast<uint8_t>(
                (registers_.f() & 0x01) | (value & 0xa8)
                | (value == 0 ? 0x40 : 0)
                | ((before & 0x0f) == (decrement ? 0 : 0x0f) ? 0x10 : 0)
                | (before == (decrement ? 0x80 : 0x7f) ? 0x04 : 0)
                | (decrement ? 0x02 : 0)));
        }
        if (destination == 6) memory->write8(operandAddress, value);
        else writeRegister8(static_cast<uint8_t>(destination), value);
    } else if ((opcode & 0xc7) == 0x07) {
        executeAccumulator(opcode);
        modifiesFlags = true;
    } else if (opcode != 0x00) {
        return std::nullopt;
    }

    if (indexedAddress) {
        memptr_ = *indexedAddress;
        ++length; // Signed displacement is an operand byte, not another M1 fetch.
        if (opcode == 0xcb) {
            refreshes = 1; // The final DDCB/FDCB opcode is not fetched with M1.
            elapsed += 4;
        } else {
            elapsed += opcode == 0x36 ? 5 : 8;
        }
    }
    flagsModified_ = modifiesFlags;
    ldAir_ = opcode == 0xed && (immediate == 0x57 || immediate == 0x5f);
    registers_.pc = branchTarget.value_or(static_cast<uint16_t>(registers_.pc + length));
    registers_.r = static_cast<uint8_t>(
        (registers_.r & 0x80) | ((registers_.r + refreshes) & 0x7f));
    if (eiDelay_ != 0) {
        --eiDelay_;
    }
    return elapsed;
}

} // namespace smsgg
