"""A small MIPS III decoder for the ROM analyser (PLAN.md 3.14): just what
function discovery and the libultra rules need, no third-party package.

Ins(word, pc) splits the fields and classifies the instruction; `valid` is
False for encodings no N64 compiler emits (reserved opcodes and functs), which
is how the analyser tells code from data."""

# primary opcodes
J, JAL, BEQ, BNE, BLEZ, BGTZ = 2, 3, 4, 5, 6, 7
ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI = 8, 9, 10, 11, 12, 13, 14, 15
COP0, COP1 = 16, 17
BEQL, BNEL, BLEZL, BGTZL = 20, 21, 22, 23
DADDI, DADDIU, LDL, LDR = 24, 25, 26, 27
LB, LH, LWL, LW, LBU, LHU, LWR, LWU = 32, 33, 34, 35, 36, 37, 38, 39
SB, SH, SWL, SW, SDL, SDR, SWR, CACHE = 40, 41, 42, 43, 44, 45, 46, 47
LL, LWC1, LLD, LDC1, LD = 48, 49, 52, 53, 55
SC, SWC1, SCD, SDC1, SD = 56, 57, 60, 61, 63

LOADS = {LB: 1, LBU: 1, LH: 2, LHU: 2, LW: 4, LWU: 4, LWL: 4, LWR: 4, LL: 4, LD: 8, LDL: 8, LDR: 8, LLD: 8,
         LWC1: 4, LDC1: 8}
STORES = {SB: 1, SH: 2, SW: 4, SWL: 4, SWR: 4, SC: 4, SD: 8, SDL: 8, SDR: 8, SCD: 8, SWC1: 4, SDC1: 8}
VALID_OPS = {0, 1, J, JAL, BEQ, BNE, BLEZ, BGTZ, ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI, COP0, COP1,
             BEQL, BNEL, BLEZL, BGTZL, DADDI, DADDIU, LDL, LDR, *LOADS, *STORES, CACHE}
# SPECIAL functs an N64 compiler or libultra's assembly emits
VALID_FUNCTS = {0, 2, 3, 4, 6, 7, 8, 9, 12, 13, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                32, 33, 34, 35, 36, 37, 38, 39, 42, 43, 44, 45, 46, 47, 48, 52, 56, 58, 59, 60, 62, 63}
SPECIAL64 = {20, 22, 23, 28, 29, 30, 31, 44, 45, 46, 47, 56, 58, 59, 60, 62, 63}   # dsllv ... dsra32
OPS64 = {DADDI, DADDIU, LD, LDL, LDR, LLD, SD, SDL, SDR, SCD}

RA, SP, A0 = 31, 29, 4


class Ins:
    __slots__ = ('w', 'pc', 'op', 'rs', 'rt', 'rd', 'sa', 'funct', 'imm', 'simm', 'target')

    def __init__(self, w, pc):
        self.w, self.pc = w, pc
        self.op = w >> 26
        self.rs, self.rt, self.rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        self.sa, self.funct = (w >> 6) & 31, w & 63
        self.imm = w & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)

    # ---- classification ----
    @property
    def valid(self):
        op = self.op
        if op not in VALID_OPS:
            return False
        if op == 0:
            if self.funct not in VALID_FUNCTS:
                return False
            if self.funct == 8 and (self.rt or self.rd):   # jr: rt and rd zero
                return False
            return True
        if op == 1:
            return self.rt in (0, 1, 2, 3, 16, 17, 18, 19)
        if op == COP0:
            return self.rs in (0, 4, 16) or self.w in (0x42000018, 0x42000001, 0x42000002, 0x42000006, 0x42000008)
        if op == COP1:
            return self.rs in (0, 1, 2, 4, 5, 6, 8, 16, 17, 20, 21)
        return True

    @property
    def nop(self):
        return self.w == 0

    @property
    def is_jal(self):
        return self.op == JAL

    @property
    def is_j(self):
        return self.op == J

    @property
    def is_jr(self):
        return self.op == 0 and self.funct == 8

    @property
    def is_jr_ra(self):
        return self.is_jr and self.rs == RA

    @property
    def is_jalr(self):
        return self.op == 0 and self.funct == 9

    @property
    def is_branch(self):
        if self.op in (BEQ, BNE, BLEZ, BGTZ, BEQL, BNEL, BLEZL, BGTZL):
            return True
        if self.op == 1 and self.rt in (0, 1, 2, 3, 16, 17, 18, 19):
            return True
        return self.op == COP1 and self.rs == 8   # bc1f / bc1t (likely too)

    @property
    def branch_target(self):
        return (self.pc + 4 + (self.simm << 2)) & 0xFFFFFFFF

    @property
    def is_unconditional_branch(self):
        """b (beq $0,$0) and bal-less bgez $0: the path does not fall through"""
        return (self.op == BEQ and self.rs == 0 and self.rt == 0) or (self.op == 1 and self.rt == 1 and self.rs == 0)

    @property
    def is_link_branch(self):   # bltzal / bgezal: a call
        return self.op == 1 and self.rt in (16, 17, 18, 19)

    @property
    def is_eret(self):
        return self.w == 0x42000018

    @property
    def mem_size(self):
        return LOADS.get(self.op) or STORES.get(self.op) or 0

    @property
    def is_load(self):
        return self.op in LOADS

    @property
    def is_store(self):
        return self.op in STORES

    @property
    def is_64(self):
        return self.op in OPS64 or (self.op == 0 and self.funct in SPECIAL64)

    @property
    def cop0(self):
        """('mf' | 'mt', register) for mfc0 / mtc0"""
        if self.op == COP0 and self.rs in (0, 4):
            return ('mf' if self.rs == 0 else 'mt', self.rd)
        return None

    @property
    def fcr(self):
        """('cf' | 'ct', register) for cfc1 / ctc1"""
        if self.op == COP1 and self.rs in (2, 6):
            return ('cf' if self.rs == 2 else 'ct', self.rd)
        return None

    @property
    def writes_rt(self):
        return self.op in (ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI, DADDI, DADDIU) or self.op in LOADS and \
            self.op not in (LWC1, LDC1)

    @property
    def dest(self):
        """the general register this instruction writes, or None"""
        if self.op == 0:
            if self.funct in (8, 24, 25, 26, 27, 28, 29, 30, 31, 52) or self.funct in (12, 13, 15, 17, 19):
                return None   # jr, mult/div, syscall/break/sync, mthi/mtlo
            return self.rd or None
        if self.writes_rt:
            return self.rt or None
        if self.op == COP0 and self.rs == 0:
            return self.rt or None
        if self.op == COP1 and self.rs in (0, 1, 2):
            return self.rt or None
        if self.op == JAL or self.is_link_branch:
            return RA
        return None


def words(data, off, n):
    """n big-endian words from bytes at off"""
    return [int.from_bytes(data[off + 4 * i:off + 4 * i + 4], 'big') for i in range(n)]
