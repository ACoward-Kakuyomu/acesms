#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace smsgg {

// The caller owns the address space used for instruction fetch and operand access.
class Z80Memory {
public:
    virtual ~Z80Memory() = default;
    virtual uint8_t read8(uint16_t address) = 0;
    virtual void write8(uint16_t address, uint8_t value) = 0;
};

class Z80Io {
public:
    virtual ~Z80Io() = default;
    virtual uint8_t readPort(uint16_t port) = 0;
    // Absolute T-state at the end of the CPU's I/O write cycle.
    virtual void writePort(uint16_t port, uint8_t value, uint64_t tstate) = 0;
};

struct Z80Registers {
    uint16_t af{};
    uint16_t bc{};
    uint16_t de{};
    uint16_t hl{};
    uint16_t afAlternate{};
    uint16_t bcAlternate{};
    uint16_t deAlternate{};
    uint16_t hlAlternate{};
    uint16_t ix{};
    uint16_t iy{};
    uint16_t sp{};
    uint16_t pc{};
    uint8_t i{};
    uint8_t r{};

    uint8_t a() const { return static_cast<uint8_t>(af >> 8); }
    uint8_t f() const { return static_cast<uint8_t>(af); }
    uint8_t b() const { return static_cast<uint8_t>(bc >> 8); }
    uint8_t c() const { return static_cast<uint8_t>(bc); }
    uint8_t d() const { return static_cast<uint8_t>(de >> 8); }
    uint8_t e() const { return static_cast<uint8_t>(de); }
    uint8_t h() const { return static_cast<uint8_t>(hl >> 8); }
    uint8_t l() const { return static_cast<uint8_t>(hl); }

    void setA(uint8_t value) { af = static_cast<uint16_t>((af & 0x00ff) | (value << 8)); }
    void setF(uint8_t value) { af = static_cast<uint16_t>((af & 0xff00) | value); }
    void setB(uint8_t value) { bc = static_cast<uint16_t>((bc & 0x00ff) | (value << 8)); }
    void setC(uint8_t value) { bc = static_cast<uint16_t>((bc & 0xff00) | value); }
    void setD(uint8_t value) { de = static_cast<uint16_t>((de & 0x00ff) | (value << 8)); }
    void setE(uint8_t value) { de = static_cast<uint16_t>((de & 0xff00) | value); }
    void setH(uint8_t value) { hl = static_cast<uint16_t>((hl & 0x00ff) | (value << 8)); }
    void setL(uint8_t value) { hl = static_cast<uint16_t>((hl & 0xff00) | value); }
};

struct Z80State {
    Z80Registers registers;
    bool iff1{}, iff2{}, halted{}, flagsModified{}, ldAir{};
    uint8_t eiDelay{}, interruptMode{};
    uint16_t memptr{};
};

enum class CpuStepError { PrefixRun };

class Z80 final {
public:
    explicit Z80(Z80Registers registers);
    const Z80Registers& registers() const noexcept;
    Z80State captureState() const noexcept;
    // Internal state from capture or the validated file boundary.
    void restoreState(const Z80State& state) noexcept;
    bool iff1() const noexcept { return iff1_; }
    bool iff2() const noexcept { return iff2_; }
    bool halted() const noexcept { return halted_; }
    uint8_t interruptMode() const noexcept { return interruptMode_; }
    bool canAcceptMaskableInterrupt() const noexcept { return iff1_ && eiDelay_ == 0; }
    void reset();
    // Fetches and executes one instruction. A full address-space prefix run is unsupported.
    std::variant<uint32_t, CpuStepError> step(Z80Memory& memory, Z80Io& io, uint64_t startTstate = 0);
    // One already-detected NMI edge, delivered at an instruction boundary.
    uint32_t acceptNmi(Z80Memory& memory);
    // 0 T-states when masked; nullopt for missing input/connections.
    std::optional<uint32_t> acceptInterrupt(
        std::span<const uint8_t> bytes, Z80Memory* memory = nullptr, Z80Io* io = nullptr, uint64_t startTstate = 0);
    // Bytes start at PC. Executes one instruction, including any DD/FD prefixes.
    // Missing bytes or unsupported operations leave all CPU state unchanged.
    std::optional<uint32_t> executeInstruction(
        std::span<const uint8_t> bytes, Z80Memory* memory = nullptr, Z80Io* io = nullptr, uint64_t startTstate = 0);
    // opcode is at the current PC; this method accounts for fetch timing/PC/R.
    // Immediate operands take the bytes at PC+1 and PC+2 (16-bit wrapping).
    // Unsupported opcodes or a missing required immediate leave state unchanged.
    std::optional<uint32_t> executeOpcode(
        uint8_t opcode, std::optional<uint8_t> immediate = std::nullopt,
        std::optional<uint8_t> immediateHigh = std::nullopt, Z80Memory* memory = nullptr, Z80Io* io = nullptr);

private:
    std::optional<uint32_t> executePrefixed(std::span<const uint8_t> instruction,
        std::size_t prefixes, uint8_t lastPrefix, Z80Memory* memory, Z80Io* io, uint64_t startTstate);
    std::optional<uint32_t> executeDecoded(uint8_t opcode, std::optional<uint8_t> immediate,
        std::optional<uint8_t> immediateHigh, std::optional<uint8_t> immediateLast, Z80Memory* memory, Z80Io* io,
        uint64_t startTstate, std::optional<uint16_t> indexedAddress = std::nullopt);
    uint16_t& registerPair(uint8_t code);
    void pushWord(Z80Memory& memory, uint16_t value);
    uint16_t popWord(Z80Memory& memory);
    uint8_t readRegister8(uint8_t code) const;
    void writeRegister8(uint8_t code, uint8_t value);
    uint8_t cb8(uint8_t opcode, uint8_t before, uint8_t xy);
    std::optional<uint32_t> executeEd(uint8_t opcode, Z80Memory* memory, Z80Io* io, uint64_t startTstate);
    uint32_t executeBlock(uint8_t opcode, Z80Memory& memory, Z80Io* io, uint64_t startTstate);
    void alu8(uint8_t operation, uint8_t operand);
    void executeAccumulator(uint8_t opcode);
    bool conditionMet(uint8_t code) const;
    Z80Registers registers_;
    bool iff1_{};
    bool iff2_{};
    bool halted_{};
    bool flagsModified_{};
    bool ldAir_{};
    uint8_t eiDelay_{};
    uint8_t interruptMode_{};
    uint16_t memptr_{};
};

} // namespace smsgg
