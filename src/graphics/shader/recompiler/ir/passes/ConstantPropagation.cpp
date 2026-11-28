#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

Value Arg(const Inst& inst, size_t index) {
	return inst.Arg(index).Resolve();
}

bool IsImmediate(Value value, Type type) {
	return value.IsImmediate() && value.GetType() == type;
}

void Replace(Inst& inst, Value value) {
	inst.ReplaceUsesWith(value.Resolve());
}

bool HasOneUse(const Inst& inst) {
	// Replacement aliases preserve external Values but emit no additional operation.
	return std::ranges::count_if(inst.Uses(), [](const Use& use) {
		return use.user->GetOpcode() != ValueOpcode::Identity;
	}) == 1;
}

class FoldWorklist {
public:
	struct Location {
		Block::iterator instruction;
		bool queued = false;
	};

	void Register(Block::iterator instruction) {
		Queue(locations.emplace(&*instruction, Location {instruction}).first->second);
	}

	void Queue(Location& location) {
		if (!location.queued) {
			location.queued = true;
			pending.push_back(&location);
		}
	}

	void QueueUsers(const std::vector<Use>& users) {
		for (const auto& use: users) affected.push_back(use.user);
	}

	void FlushUsers() {
		// Pattern folds inspect nested operands even when intermediate operations
		// stay dynamic. Visit their union once per drain, including cyclic Phi edges;
		// the drain left nothing queued, so a queued instruction was visited here.
		for (size_t i = 0; i < affected.size(); ++i) {
			auto* inst = affected[i];
			const auto found = locations.find(inst);
			if (found == locations.end() || found->second.queued) continue;
			Queue(found->second);
			for (const auto& use: inst->Uses()) affected.push_back(use.user);
		}
		affected.clear();
	}

	Block::iterator Prepend(Block& block, Block::iterator before, ValueOpcode opcode,
	                         std::initializer_list<Value> args = {}) {
		const auto result = block.PrependNewInst(before, opcode, args);
		Register(result);
		return result;
	}

	std::unordered_map<Inst*, Location> locations;
	std::vector<Location*>              pending; // Map nodes keep their addresses across rehashes.
	std::unordered_set<Inst*> lowered_ancillary;

private:
	std::vector<Inst*> affected;
};

// Reads an immediate operand of a fold function's parameter type.
template <typename T>
bool ReadImmediate(Value value, T& result) {
	if constexpr (std::is_same_v<T, bool>) {
		if (!IsImmediate(value, Type::U1)) return false;
		result = value.U1();
	} else if constexpr (std::is_same_v<T, uint32_t>) {
		if (!IsImmediate(value, Type::U32)) return false;
		result = value.U32();
	} else {
		static_assert(std::is_same_v<T, uint64_t>);
		if (!IsImmediate(value, Type::U64)) return false;
		result = value.U64();
	}
	return true;
}

// Folds a binary operation on two immediates of the function's parameter types.
template <typename Function, typename Result, typename Lhs, typename Rhs>
bool FoldBinary(Inst& inst, Function function, Result (Function::*)(Lhs, Rhs) const) {
	Lhs lhs {};
	Rhs rhs {};
	if (!ReadImmediate(Arg(inst, 0), lhs) || !ReadImmediate(Arg(inst, 1), rhs)) {
		return false;
	}
	Replace(inst, Value(function(lhs, rhs)));
	return true;
}

template <typename Function>
bool FoldBinary(Inst& inst, Function function) {
	return FoldBinary(inst, function, &Function::operator());
}

bool ReplaceBinaryIdentity(Inst& inst, Type type, uint64_t identity) {
	const auto lhs = Arg(inst, 0);
	const auto rhs = Arg(inst, 1);
	if (IsImmediate(lhs, type) &&
	    (type == Type::U32 ? lhs.U32() == identity : lhs.U64() == identity)) {
		Replace(inst, rhs);
		return true;
	}
	if (IsImmediate(rhs, type) &&
	    (type == Type::U32 ? rhs.U32() == identity : rhs.U64() == identity)) {
		Replace(inst, lhs);
		return true;
	}
	return false;
}

// Whether one operand is the other's negation.
bool IsComplementPair(const Inst& inst, ValueOpcode negation) {
	for (uint32_t arg = 0; arg < 2u; ++arg) {
		const auto* negate = Arg(inst, arg).TryInstruction();
		if (negate != nullptr && negate->GetOpcode() == negation &&
		    Arg(*negate, 0) == Arg(inst, arg ^ 1u)) return true;
	}
	return false;
}

struct BoolFact {
	Value value;
	bool  sense = true;

	bool operator==(const BoolFact& other) const = default;
};

void CollectBoolFacts(Value condition, bool sense, std::vector<BoolFact>& out, int depth = 0) {
	if (depth >= 16 || out.size() >= 32u) return;
	condition = condition.Resolve();
	if (IsImmediate(condition, Type::U1)) return;
	if (const auto* inst = condition.TryInstruction(); inst != nullptr) {
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::LogicalNot) {
			CollectBoolFacts(Arg(*inst, 0), !sense, out, depth + 1);
			return;
		}
		if ((sense && op == ValueOpcode::LogicalAnd) ||
		    (!sense && op == ValueOpcode::LogicalOr)) {
			CollectBoolFacts(Arg(*inst, 0), sense, out, depth + 1);
			CollectBoolFacts(Arg(*inst, 1), sense, out, depth + 1);
			return;
		}
	}
	const BoolFact fact {condition, sense};
	if (std::ranges::find(out, fact) == out.end()) out.push_back(fact);
}

bool ImpliesBoolValue(std::span<const BoolFact> facts, Value target, bool expected,
                      int depth = 0) {
	target = target.Resolve();
	if (IsImmediate(target, Type::U1)) return target.U1() == expected;
	if (depth >= 12) return false;
	for (const auto& fact: facts) {
		if (fact.value == target && fact.sense == expected) return true;
	}
	const auto* inst = target.TryInstruction();
	if (inst == nullptr) return false;
	switch (inst->GetOpcode()) {
		case ValueOpcode::LogicalNot:
			return ImpliesBoolValue(facts, Arg(*inst, 0), !expected, depth + 1);
		case ValueOpcode::LogicalAnd:
			return expected
			           ? ImpliesBoolValue(facts, Arg(*inst, 0), true, depth + 1) &&
			                 ImpliesBoolValue(facts, Arg(*inst, 1), true, depth + 1)
			           : ImpliesBoolValue(facts, Arg(*inst, 0), false, depth + 1) ||
			                 ImpliesBoolValue(facts, Arg(*inst, 1), false, depth + 1);
		case ValueOpcode::LogicalOr:
			return expected
			           ? ImpliesBoolValue(facts, Arg(*inst, 0), true, depth + 1) ||
			                 ImpliesBoolValue(facts, Arg(*inst, 1), true, depth + 1)
			           : ImpliesBoolValue(facts, Arg(*inst, 0), false, depth + 1) &&
			                 ImpliesBoolValue(facts, Arg(*inst, 1), false, depth + 1);
		default: return false;
	}
}

bool FoldSelect(Inst& inst, bool aggressive) {
	const auto condition   = Arg(inst, 0);
	const auto true_value  = Arg(inst, 1);
	const auto false_value = Arg(inst, 2);
	if (IsImmediate(condition, Type::U1)) {
		Replace(inst, condition.U1() ? true_value : false_value);
		return true;
	}
	if (true_value == false_value) {
		Replace(inst, true_value);
		return true;
	}
	if (!aggressive) return false;
	const auto op = inst.GetOpcode();
	if (const auto* inner_true = true_value.TryInstruction();
	    inner_true != nullptr && inner_true->GetOpcode() == op) {
		std::vector<BoolFact> facts;
		CollectBoolFacts(condition, true, facts);
		const auto inner_cond = Arg(*inner_true, 0);
		if (ImpliesBoolValue(facts, inner_cond, true)) {
			inst.SetArg(1, Arg(*inner_true, 1));
		} else if (ImpliesBoolValue(facts, inner_cond, false)) {
			inst.SetArg(1, Arg(*inner_true, 2));
		}
	}
	if (const auto* inner_false = Arg(inst, 2).TryInstruction();
	    inner_false != nullptr && inner_false->GetOpcode() == op) {
		std::vector<BoolFact> facts;
		CollectBoolFacts(condition, false, facts);
		const auto inner_cond = Arg(*inner_false, 0);
		if (ImpliesBoolValue(facts, inner_cond, true)) {
			inst.SetArg(2, Arg(*inner_false, 1));
		} else if (ImpliesBoolValue(facts, inner_cond, false)) {
			inst.SetArg(2, Arg(*inner_false, 2));
		}
	}
	if (Arg(inst, 1) == Arg(inst, 2)) {
		Replace(inst, Arg(inst, 1));
		return true;
	}
	if (aggressive) {
		const auto t_val = Arg(inst, 1);
		const auto f_val = Arg(inst, 2);
		if (const auto* inner = t_val.TryInstruction(); inner != nullptr && inner->GetOpcode() == op) {
			const auto inner_c = Arg(*inner, 0);
			const auto inner_t = Arg(*inner, 1);
			const auto inner_f = Arg(*inner, 2);
			if (inner_f == f_val) {
				std::vector<BoolFact> facts;
				CollectBoolFacts(inner_c, true, facts);
				if (ImpliesBoolValue(facts, condition, true)) {
					Replace(inst, t_val);
					return true;
				}
			}
			if (inner_t == f_val) {
				std::vector<BoolFact> facts;
				CollectBoolFacts(inner_c, false, facts);
				if (ImpliesBoolValue(facts, condition, true)) {
					Replace(inst, t_val);
					return true;
				}
			}
		}
		if (const auto* inner = f_val.TryInstruction(); inner != nullptr && inner->GetOpcode() == op) {
			const auto inner_c = Arg(*inner, 0);
			const auto inner_t = Arg(*inner, 1);
			const auto inner_f = Arg(*inner, 2);
			if (inner_t == t_val) {
				std::vector<BoolFact> facts;
				CollectBoolFacts(condition, true, facts);
				if (ImpliesBoolValue(facts, inner_c, true)) {
					Replace(inst, f_val);
					return true;
				}
			}
			if (inner_f == t_val) {
				std::vector<BoolFact> facts;
				CollectBoolFacts(condition, true, facts);
				if (ImpliesBoolValue(facts, inner_c, false)) {
					Replace(inst, f_val);
					return true;
				}
			}
		}
	}
	return false;
}

struct U32Range {
	uint32_t min    = 0u;
	uint32_t max    = UINT32_MAX;
	uint32_t stride = 1u;

	bool operator==(const U32Range&) const = default;
};

constexpr U32Range FullU32Range {};

U32Range ConstU32Range(uint32_t value) {
	return {value, value, value};
}

U32Range JoinU32Ranges(U32Range a, U32Range b) {
	return {std::min(a.min, b.min), std::max(a.max, b.max), std::gcd(a.stride, b.stride)};
}

U32Range RangeForBitAnd(U32Range lhs, U32Range rhs) {
	if (lhs.min == lhs.max && rhs.min == rhs.max) {
		return ConstU32Range(lhs.min & rhs.min);
	}
	const uint32_t max_val = std::min(lhs.max, rhs.max);
	return {0u, max_val, max_val == 0u ? 0u : 1u};
}

U32Range RangeForBitOr(U32Range lhs, U32Range rhs) {
	if (lhs.min == lhs.max && rhs.min == rhs.max) {
		return ConstU32Range(lhs.min | rhs.min);
	}
	const auto mask_above = [](uint32_t bound) {
		return bound == 0u ? 0u : (UINT32_MAX >> std::countl_zero(bound));
	};
	const uint32_t max_val = mask_above(lhs.max) | mask_above(rhs.max);
	return {std::max(lhs.min, rhs.min), max_val, max_val == 0u ? 0u : 1u};
}

U32Range RangeForBfeU32(U32Range source, Value offset, Value count) {
	if (!IsImmediate(count, Type::U32)) return FullU32Range;
	const uint32_t c = count.U32();
	if (c == 0u) return ConstU32Range(0u);
	if (IsImmediate(offset, Type::U32)) {
		const uint32_t off = offset.U32();
		if (off == 0u && c == 32u) return source;
		if (off <= 32u && c <= 32u - off) {
			const uint32_t mask    = c == 32u ? UINT32_MAX : (uint32_t {1} << c) - 1u;
			const uint32_t shifted = off == 32u ? 0u : (source.max >> off);
			const uint32_t max_val = std::min(mask, shifted);
			return {0u, max_val, max_val == 0u ? 0u : 1u};
		}
		return FullU32Range;
	}
	if (c < 32u) {
		const uint32_t mask = (uint32_t {1} << c) - 1u;
		return {0u, mask, 1u};
	}
	return FullU32Range;
}

U32Range RangeForShrU32(U32Range source, Value shift) {
	if (!IsImmediate(shift, Type::U32)) return FullU32Range;
	const uint32_t amount = shift.U32() & 31u;
	if (amount == 0u) return source;
	const uint32_t max_val = source.max >> amount;
	return {source.min >> amount, max_val, max_val == 0u ? 0u : 1u};
}

U32Range RangeForShlU32(U32Range source, Value shift) {
	if (!IsImmediate(shift, Type::U32)) return FullU32Range;
	const uint32_t amount = shift.U32() & 31u;
	if (amount == 0u) return source;
	const uint64_t smax = uint64_t {source.max} << amount;
	if (smax > UINT32_MAX) return FullU32Range;
	if (source.max == 0u) return ConstU32Range(0u);
	const uint64_t stride64 = uint64_t {std::max(source.stride, 1u)} << amount;
	const uint32_t stride   = stride64 <= UINT32_MAX ? static_cast<uint32_t>(stride64) : 1u;
	return {source.min << amount, static_cast<uint32_t>(smax), stride};
}

U32Range RangeForAddU32(U32Range lhs, U32Range rhs) {
	const uint64_t smax = uint64_t {lhs.max} + rhs.max;
	if (smax > UINT32_MAX) return FullU32Range;
	return {lhs.min + rhs.min, static_cast<uint32_t>(smax), std::gcd(lhs.stride, rhs.stride)};
}

U32Range RangeForMulU32(U32Range lhs, U32Range rhs) {
	const uint64_t smax = uint64_t {lhs.max} * rhs.max;
	if (smax > UINT32_MAX) return FullU32Range;
	if (smax == 0u) return ConstU32Range(0u);
	const uint64_t stride64 = uint64_t {lhs.stride} * rhs.stride;
	const uint32_t stride   = stride64 <= UINT32_MAX ? static_cast<uint32_t>(stride64) : 1u;
	return {lhs.min * rhs.min, static_cast<uint32_t>(smax), stride};
}

std::optional<bool> EvaluateRangeCompare(ValueOpcode op, U32Range a, U32Range b) {
	switch (op) {
		case ValueOpcode::IEqual32:
		case ValueOpcode::INotEqual32: {
			const bool eq = op == ValueOpcode::IEqual32;
			if (a.max < b.min || b.max < a.min) return !eq;
			if (a.min == a.max && b.min == b.max && a.min == b.min) return eq;
			if (b.min == b.max && a.stride > 1u && (b.min % a.stride) != 0u) return !eq;
			if (a.min == a.max && b.stride > 1u && (a.min % b.stride) != 0u) return !eq;
			return std::nullopt;
		}
		case ValueOpcode::ULessThan32:
			if (a.max < b.min) return true;
			if (a.min >= b.max) return false;
			return std::nullopt;
		case ValueOpcode::ULessThanEqual32:
			if (a.max <= b.min) return true;
			if (a.min > b.max) return false;
			return std::nullopt;
		case ValueOpcode::UGreaterThan32:
			if (a.min > b.max) return true;
			if (a.max <= b.min) return false;
			return std::nullopt;
		case ValueOpcode::UGreaterThanEqual32:
			if (a.min >= b.max) return true;
			if (a.max < b.min) return false;
			return std::nullopt;
		default: return std::nullopt;
	}
}

U32Range ComputeU32Range(Value value, int depth = 0) {
	value = value.Resolve();
	if (IsImmediate(value, Type::U32)) return ConstU32Range(value.U32());
	if (depth >= 10) return FullU32Range;
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return FullU32Range;
	switch (inst->GetOpcode()) {
		case ValueOpcode::BitwiseAnd32:
			return RangeForBitAnd(ComputeU32Range(Arg(*inst, 0), depth + 1),
			                      ComputeU32Range(Arg(*inst, 1), depth + 1));
		case ValueOpcode::BitwiseOr32:
			return RangeForBitOr(ComputeU32Range(Arg(*inst, 0), depth + 1),
			                     ComputeU32Range(Arg(*inst, 1), depth + 1));
		case ValueOpcode::BitFieldUExtract:
			return RangeForBfeU32(ComputeU32Range(Arg(*inst, 0), depth + 1), Arg(*inst, 1),
			                      Arg(*inst, 2));
		case ValueOpcode::ShiftRightLogical32:
			return RangeForShrU32(ComputeU32Range(Arg(*inst, 0), depth + 1), Arg(*inst, 1));
		case ValueOpcode::ShiftLeftLogical32:
			return RangeForShlU32(ComputeU32Range(Arg(*inst, 0), depth + 1), Arg(*inst, 1));
		case ValueOpcode::UMin32: {
			const auto lhs = ComputeU32Range(Arg(*inst, 0), depth + 1);
			const auto rhs = ComputeU32Range(Arg(*inst, 1), depth + 1);
			const auto max_val = std::min(lhs.max, rhs.max);
			return {std::min(lhs.min, rhs.min), max_val, max_val == 0u ? 0u : 1u};
		}
		case ValueOpcode::UMax32: {
			const auto lhs = ComputeU32Range(Arg(*inst, 0), depth + 1);
			const auto rhs = ComputeU32Range(Arg(*inst, 1), depth + 1);
			const auto max_val = std::max(lhs.max, rhs.max);
			return {std::max(lhs.min, rhs.min), max_val, max_val == 0u ? 0u : 1u};
		}
		case ValueOpcode::SelectU32:
			return JoinU32Ranges(ComputeU32Range(Arg(*inst, 1), depth + 1),
			                     ComputeU32Range(Arg(*inst, 2), depth + 1));
		case ValueOpcode::ConvertU32U8: return {0u, 0xffu, 1u};
		case ValueOpcode::ConvertU32U16: return {0u, 0xffffu, 1u};
		case ValueOpcode::IAdd32:
			return RangeForAddU32(ComputeU32Range(Arg(*inst, 0), depth + 1),
			                      ComputeU32Range(Arg(*inst, 1), depth + 1));
		case ValueOpcode::IMul32:
			return RangeForMulU32(ComputeU32Range(Arg(*inst, 0), depth + 1),
			                      ComputeU32Range(Arg(*inst, 1), depth + 1));
		case ValueOpcode::ReadFirstLane:
		case ValueOpcode::ReadLane: return ComputeU32Range(Arg(*inst, 0), depth + 1);
		default: return FullU32Range;
	}
}

bool FoldBoundedU32Compare(Inst& inst) {
	const auto op  = inst.GetOpcode();
	const auto lhs = Arg(inst, 0);
	const auto rhs = Arg(inst, 1);
	if (lhs == rhs) {
		const bool res = op == ValueOpcode::IEqual32 || op == ValueOpcode::ULessThanEqual32 ||
		                 op == ValueOpcode::UGreaterThanEqual32;
		Replace(inst, Value(res));
		return true;
	}
	if (const auto res = EvaluateRangeCompare(op, ComputeU32Range(lhs), ComputeU32Range(rhs))) {
		Replace(inst, Value(*res));
		return true;
	}
	return false;
}

bool FoldPhi(Inst& inst) {
	Value same;
	for (size_t index = 0; index < inst.NumArgs(); index++) {
		const auto value = Arg(inst, index);
		if (value.TryInstruction() == &inst) {
			continue;
		}
		if (same.IsEmpty()) {
			same = value;
		} else if (same != value) {
			return false;
		}
	}
	if (same.IsEmpty()) {
		return false;
	}
	Replace(inst, same);
	return true;
}

bool FoldBitCast(Inst& inst, ValueOpcode reverse) {
	const auto value = Arg(inst, 0);
	if (IsImmediate(value, Type::F32) && inst.GetOpcode() == ValueOpcode::BitCastU32F32) {
		Replace(inst, Value(std::bit_cast<uint32_t>(value.F32Value())));
		return true;
	}
	if (IsImmediate(value, Type::U32) && inst.GetOpcode() == ValueOpcode::BitCastF32U32) {
		Replace(inst, Value::F32(std::bit_cast<float>(value.U32())));
		return true;
	}
	if (IsImmediate(value, Type::F16) && inst.GetOpcode() == ValueOpcode::BitCastU16F16) {
		Replace(inst, Value(value.F16Bits()));
		return true;
	}
	if (IsImmediate(value, Type::U16) && inst.GetOpcode() == ValueOpcode::BitCastF16U16) {
		Replace(inst, Value::F16(value.U16()));
		return true;
	}
	if (auto* producer = value.TryInstruction();
	    producer != nullptr && producer->GetOpcode() == reverse) {
		Replace(inst, producer->Arg(0));
		return true;
	}
	return false;
}

bool SinkBitCastIntoSelect(Inst& inst, FoldWorklist& worklist, ValueOpcode select_op,
                           ValueOpcode cast_op, ValueOpcode reverse_op, Type arm_imm_type) {
	auto* source = Arg(inst, 0).TryInstruction();
	if (source == nullptr || source->GetOpcode() != select_op) return false;
	const bool all_matching_casts = std::ranges::all_of(source->Uses(), [&](const Use& use) {
		const auto op = use.user->GetOpcode();
		return op == ValueOpcode::Identity || op == cast_op;
	});
	if (!all_matching_casts) return false;
	const auto is_target_arm = [&](Value value) {
		value = value.Resolve();
		if (IsImmediate(value, arm_imm_type)) return true;
		const auto* producer = value.TryInstruction();
		return producer != nullptr && producer->GetOpcode() == reverse_op;
	};
	if (!is_target_arm(Arg(*source, 1)) && !is_target_arm(Arg(*source, 2))) return false;
	auto*      src_block = source->Parent();
	const auto after_src = std::next(worklist.locations.at(source).instruction);
	const auto bt = worklist.Prepend(*src_block, after_src, cast_op, {Arg(*source, 1)});
	const auto bf = worklist.Prepend(*src_block, after_src, cast_op, {Arg(*source, 2)});
	const auto target_select = select_op == ValueOpcode::SelectU32 ? ValueOpcode::SelectF32
	                                                               : ValueOpcode::SelectU32;
	const auto sel = worklist.Prepend(*src_block, after_src, target_select,
	                                  {Arg(*source, 0), Value(&*bt), Value(&*bf)});
	const std::vector<Use> src_uses(source->Uses().begin(), source->Uses().end());
	for (const auto& use: src_uses) {
		if (use.user->GetOpcode() == cast_op) {
			worklist.QueueUsers(use.user->Uses());
			Replace(*use.user, Value(&*sel));
		}
	}
	return true;
}

bool FoldCompositeExtract(Inst& inst, ValueOpcode construct, size_t components) {
	const auto composite = Arg(inst, 0);
	const auto index     = Arg(inst, 1);
	if (!IsImmediate(index, Type::U32) || index.U32() >= components) {
		return false;
	}
	const auto  component = index.U32();
	const auto* producer  = composite.TryInstruction();
	if (IsImmediate(composite, Type::U64)) {
		Replace(inst, Value(static_cast<uint32_t>(composite.U64() >> (component * 32u))));
		return true;
	}
	if (producer != nullptr && producer->GetOpcode() == construct) {
		Replace(inst, producer->Arg(component));
		return true;
	}
	return false;
}

void FoldZeroComparison(Block& block, Block::iterator instruction, FoldWorklist& worklist) {
	auto& inst = *instruction;
	Value value;
	for (uint32_t arg = 0; arg < 2u; ++arg)
		if (Arg(inst, arg) == Value(0u)) value = Arg(inst, arg ^ 1u);
	const auto* source = value.TryInstruction();
	if (source == nullptr) return;
	const bool equal = inst.GetOpcode() == ValueOpcode::IEqual32;
	const auto boolean_select = [](const Inst* select) {
		return select != nullptr && select->GetOpcode() == ValueOpcode::SelectU32 &&
		       IsImmediate(Arg(*select, 1), Type::U32) &&
		       IsImmediate(Arg(*select, 2), Type::U32);
	};
	if (boolean_select(source)) {
		const bool when_true = (Arg(*source, 1).U32() == 0u) == equal;
		const bool when_false = (Arg(*source, 2).U32() == 0u) == equal;
		if (when_true == when_false) Replace(inst, Value(when_true));
		else if (when_true) Replace(inst, Arg(*source, 0));
		else Replace(inst, Value(&*worklist.Prepend(block, instruction, ValueOpcode::LogicalNot,
		                                          {Arg(*source, 0)})));
	} else if (source->GetOpcode() == ValueOpcode::BitwiseOr32 &&
	           (boolean_select(Arg(*source, 0).TryInstruction()) ||
	            boolean_select(Arg(*source, 1).TryInstruction()))) {
		// SCC encoded as a DWORD needs no select or bitwise operation when only
		// its zero predicate is consumed. Keep arbitrary DWORDs as zero comparisons.
		const auto lhs = worklist.Prepend(block, instruction, inst.GetOpcode(),
		                                 {Arg(*source, 0), Value(0u)});
		const auto rhs = worklist.Prepend(block, instruction, inst.GetOpcode(),
		                                 {Arg(*source, 1), Value(0u)});
		Replace(inst, Value(&*worklist.Prepend(block, instruction,
		    equal ? ValueOpcode::LogicalAnd : ValueOpcode::LogicalOr, {Value(&*lhs), Value(&*rhs)})));
	}
}

void FoldInstruction(Block& block, Block::iterator instruction,
	                      FoldWorklist& worklist, bool aggressive) {
	auto& inst = *instruction;
	switch (inst.GetOpcode()) {
		case ValueOpcode::StoreBufferU32: {
			const auto data = ResolveActiveU32(Arg(inst, 4), Arg(inst, 5));
			if (!data.IsEmpty() && data != inst.Arg(4)) inst.SetArg(4, data);
			return;
		}
		case ValueOpcode::Phi: FoldPhi(inst); return;
		case ValueOpcode::SelectU1:
			if (!FoldSelect(inst, aggressive) && IsImmediate(Arg(inst, 2), Type::U1) &&
			    !Arg(inst, 2).U1()) {
				auto result = worklist.Prepend(block, instruction, ValueOpcode::LogicalAnd,
				                                   {Arg(inst, 0), Arg(inst, 1)});
				Replace(inst, Value(&*result));
			}
			return;
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32: FoldSelect(inst, aggressive); return;
		case ValueOpcode::FPCmpClass32: {
			const auto mask = Arg(inst, 1);
			if (!IsImmediate(mask, Type::U32)) {
				return;
			}
			switch (mask.U32() & 0x3ffu) {
				case 0u: Replace(inst, Value(false)); break;
				case 0x3ffu: Replace(inst, Value(true)); break;
				case 3u: {
					const auto result = worklist.Prepend(
					    block, instruction, ValueOpcode::FPIsNan32, {Arg(inst, 0)});
					Replace(inst, Value(&*result));
					break;
				}
				default: break;
			}
			return;
		}
		case ValueOpcode::BitFieldInsert: {
			const auto base   = Arg(inst, 0);
			const auto insert = Arg(inst, 1);
			const auto offset = Arg(inst, 2);
			const auto count  = Arg(inst, 3);
			if (IsImmediate(base, Type::U32) && IsImmediate(insert, Type::U32) &&
			    IsImmediate(offset, Type::U32) && IsImmediate(count, Type::U32) &&
			    offset.U32() <= 32u && count.U32() <= 32u - offset.U32()) {
				if (count.U32() == 0u) {
					Replace(inst, base);
					return;
				}
				const auto mask = count.U32() == 32u
				                      ? UINT32_MAX
				                      : ((uint32_t {1} << count.U32()) - 1u) << offset.U32();
				Replace(inst,
				        Value((base.U32() & ~mask) | ((insert.U32() << offset.U32()) & mask)));
			}
			return;
		}
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract: {
			const auto value  = Arg(inst, 0);
			const auto offset = Arg(inst, 1);
			const auto count  = Arg(inst, 2);
			auto* source = value.TryInstruction();
			if (source != nullptr && source->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
			    IsImmediate(offset, Type::U32) && IsImmediate(count, Type::U32)) {
				const auto shift = Arg(*source, 1);
				if (IsImmediate(shift, Type::U32) && shift.U32() < 32u &&
				    offset.U32() <= shift.U32() && count.U32() <= shift.U32() - offset.U32()) {
					Replace(inst, Value(0u));
					return;
				}
			}
			if (source != nullptr && source->GetOpcode() == ValueOpcode::GetBuiltin &&
			    source->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::PackedAncillary)) &&
			    IsImmediate(offset, Type::U32) && IsImmediate(count, Type::U32) && count.U32() != 0u) {
				constexpr struct {
					uint32_t       start;
					uint32_t       end;
					StageInputKind kind;
				} fields[] = {{8u, 12u, StageInputKind::SampleId}, {16u, 27u, StageInputKind::Layer}};
				for (const auto& field: fields) {
					if (offset.U32() >= field.start && offset.U32() < field.end &&
					    count.U32() <= field.end - offset.U32()) {
						// Preserve extraction and sign extension while exposing only the used field.
						const auto input = worklist.Prepend(
						    block, instruction, ValueOpcode::GetBuiltin,
						    {Value(static_cast<uint32_t>(field.kind)), Value(0u)});
						inst.SetArg(0, Value(&*input));
						inst.SetArg(1, Value(offset.U32() - field.start));
						worklist.lowered_ancillary.insert(source);
						return;
					}
				}
			}
			if (!IsImmediate(value, Type::U32) || !IsImmediate(offset, Type::U32) ||
			    !IsImmediate(count, Type::U32) || offset.U32() > 32u ||
			    count.U32() > 32u - offset.U32()) {
				return;
			}
			if (count.U32() == 0u) {
				Replace(inst, Value(0u));
			} else if (inst.GetOpcode() == ValueOpcode::BitFieldUExtract) {
				const auto mask =
				    count.U32() == 32u ? UINT32_MAX : (uint32_t {1} << count.U32()) - 1u;
				Replace(inst, Value((value.U32() >> offset.U32()) & mask));
			} else {
				const auto left = 32u - offset.U32() - count.U32();
				const auto bits = value.U32() << left;
				Replace(inst, Value(static_cast<uint32_t>(std::bit_cast<int32_t>(bits) >>
				                                          (left + offset.U32()))));
			}
			return;
		}
		case ValueOpcode::BitCastU16F16: FoldBitCast(inst, ValueOpcode::BitCastF16U16); return;
		case ValueOpcode::BitCastF16U16: FoldBitCast(inst, ValueOpcode::BitCastU16F16); return;
		case ValueOpcode::BitCastU32F32:
			if (!FoldBitCast(inst, ValueOpcode::BitCastF32U32) && aggressive) {
				SinkBitCastIntoSelect(inst, worklist, ValueOpcode::SelectF32,
				                      ValueOpcode::BitCastU32F32, ValueOpcode::BitCastF32U32,
				                      Type::F32);
			}
			return;
		case ValueOpcode::BitCastF32U32:
			if (!FoldBitCast(inst, ValueOpcode::BitCastU32F32) && aggressive) {
				SinkBitCastIntoSelect(inst, worklist, ValueOpcode::SelectU32,
				                      ValueOpcode::BitCastF32U32, ValueOpcode::BitCastU32F32,
				                      Type::U32);
			}
			return;
		case ValueOpcode::ConvertU16U32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst, Value(static_cast<uint16_t>(value.U32())));
			}
			return;
		}
		case ValueOpcode::ConvertU32U16: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U16)) {
				Replace(inst, Value(static_cast<uint32_t>(value.U16())));
			} else if (auto* producer = value.TryInstruction();
			           producer != nullptr && producer->GetOpcode() == ValueOpcode::ConvertU16U32) {
				Replace(inst, producer->Arg(0));
			}
			return;
		}
		case ValueOpcode::ConvertU8U32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst, Value(static_cast<uint8_t>(value.U32())));
			}
			return;
		}
		case ValueOpcode::ConvertU32U8: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U8)) {
				Replace(inst, Value(static_cast<uint32_t>(value.U8())));
			} else if (auto* producer = value.TryInstruction();
			           producer != nullptr && producer->GetOpcode() == ValueOpcode::ConvertU8U32) {
				Replace(inst, producer->Arg(0));
			}
			return;
		}
		case ValueOpcode::CompositeExtractU64:
			FoldCompositeExtract(inst, ValueOpcode::CompositeConstructU64, 2);
			return;
		case ValueOpcode::CompositeExtractU32x2:
			FoldCompositeExtract(inst, ValueOpcode::CompositeConstructU32x2, 2);
			return;
		case ValueOpcode::CompositeExtractU32x3:
			FoldCompositeExtract(inst, ValueOpcode::CompositeConstructU32x3, 3);
			return;
		case ValueOpcode::CompositeExtractU32x4:
			FoldCompositeExtract(inst, ValueOpcode::CompositeConstructU32x4, 4);
			return;
		case ValueOpcode::CompositeConstructU64: {
			const auto low  = Arg(inst, 0);
			const auto high = Arg(inst, 1);
			if (IsImmediate(low, Type::U32) && IsImmediate(high, Type::U32)) {
				Replace(inst, Value(static_cast<uint64_t>(low.U32()) |
				                    (static_cast<uint64_t>(high.U32()) << 32u)));
			} else {
				const auto* low_extract  = low.TryInstruction();
				const auto* high_extract = high.TryInstruction();
				if (low_extract != nullptr && high_extract != nullptr &&
				    low_extract->GetOpcode() == ValueOpcode::CompositeExtractU64 &&
				    high_extract->GetOpcode() == ValueOpcode::CompositeExtractU64 &&
				    Arg(*low_extract, 1) == Value(0u) && Arg(*high_extract, 1) == Value(1u) &&
				    Arg(*low_extract, 0) == Arg(*high_extract, 0)) {
					// Guest register pairs need not unpack and repack between wide operations.
					Replace(inst, Arg(*low_extract, 0));
				}
			}
			return;
		}
		case ValueOpcode::IAdd32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a + b; })) {
				ReplaceBinaryIdentity(inst, Type::U32, 0u);
			}
			return;
		case ValueOpcode::IAdd64:
			if (!FoldBinary(inst, [](uint64_t a, uint64_t b) { return a + b; })) {
				ReplaceBinaryIdentity(inst, Type::U64, 0u);
			}
			return;
		case ValueOpcode::ISub32: {
			if (FoldBinary(inst, [](uint32_t a, uint32_t b) { return a - b; })) {
				return;
			}
			const auto rhs = Arg(inst, 1);
			if (IsImmediate(rhs, Type::U32) && rhs.U32() == 0u) {
				Replace(inst, Arg(inst, 0));
			}
			return;
		}
		case ValueOpcode::ISub64: {
			if (FoldBinary(inst, [](uint64_t a, uint64_t b) { return a - b; })) {
				return;
			}
			const auto rhs = Arg(inst, 1);
			if (IsImmediate(rhs, Type::U64) && rhs.U64() == 0u) {
				Replace(inst, Arg(inst, 0));
			}
			return;
		}
		case ValueOpcode::IMul32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a * b; })) {
				ReplaceBinaryIdentity(inst, Type::U32, 1u);
			}
			return;
		case ValueOpcode::WqmU64: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U64)) {
				const auto expand = [](uint32_t word) {
					auto quads = word | (word >> 1u);
					quads |= quads >> 2u;
					return (quads & 0x11111111u) * 0x0fu;
				};
				const auto low  = expand(static_cast<uint32_t>(value.U64()));
				const auto high = expand(static_cast<uint32_t>(value.U64() >> 32u));
				Replace(inst,
				        Value(static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32u)));
			}
			return;
		}
		case ValueOpcode::UDiv32: {
			const auto rhs = Arg(inst, 1);
			if (IsImmediate(rhs, Type::U32) && rhs.U32() == 1u) {
				Replace(inst, Arg(inst, 0));
			} else if (IsImmediate(rhs, Type::U32) && rhs.U32() != 0u) {
				FoldBinary(inst, [](uint32_t a, uint32_t b) { return a / b; });
			}
			return;
		}
		case ValueOpcode::SMulHi:
			FoldBinary(inst, [](uint32_t a, uint32_t b) {
				const auto product = static_cast<int64_t>(std::bit_cast<int32_t>(a)) *
				                     static_cast<int64_t>(std::bit_cast<int32_t>(b));
				return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32u);
			});
			return;
		case ValueOpcode::UMulHi:
			FoldBinary(inst, [](uint32_t a, uint32_t b) {
				return static_cast<uint32_t>((static_cast<uint64_t>(a) * b) >> 32u);
			});
			return;
		case ValueOpcode::IAbs32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst,
				        Value((value.U32() & 0x80000000u) != 0u ? 0u - value.U32() : value.U32()));
			}
			return;
		}
		case ValueOpcode::ShiftLeftLogical32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a << (b & 31u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 31u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightLogical32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a >> (b & 31u); })) {
				if (IsImmediate(Arg(inst, 0), Type::U32) && Arg(inst, 0).U32() == 0u) {
					Replace(inst, Value(0u));
					return;
				}
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 31u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightArithmetic32:
			FoldBinary(inst, [](uint32_t a, uint32_t b) {
				return static_cast<uint32_t>(std::bit_cast<int32_t>(a) >> (b & 31u));
			});
			return;
		case ValueOpcode::ShiftLeftLogical64:
			if (!FoldBinary(inst, [](uint64_t a, uint32_t b) { return a << (b & 63u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 63u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightLogical64:
			if (!FoldBinary(inst, [](uint64_t a, uint32_t b) { return a >> (b & 63u); })) {
				const auto shift = Arg(inst, 1);
				if (IsImmediate(shift, Type::U32) && (shift.U32() & 63u) == 0u) {
					Replace(inst, Arg(inst, 0));
				}
			}
			return;
		case ValueOpcode::ShiftRightArithmetic64:
			FoldBinary(inst, [](uint64_t a, uint32_t b) {
				return static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u));
			});
			return;
		case ValueOpcode::BitwiseAnd32:
			if (FoldBinary(inst, [](uint32_t a, uint32_t b) { return a & b; }) ||
			    ReplaceBinaryIdentity(inst, Type::U32, UINT32_MAX)) return;
			for (uint32_t arm = 0; arm < 2u; ++arm) {
				const auto mask = Arg(inst, arm);
				if (!IsImmediate(mask, Type::U32)) continue;
				if (mask.U32() == 0u) {
					Replace(inst, Value(0u));
					return;
				}
				const auto* shifted = Arg(inst, arm ^ 1u).TryInstruction();
				if (shifted == nullptr || shifted->GetOpcode() != ValueOpcode::ShiftLeftLogical32) continue;
				const auto shift = Arg(*shifted, 1);
				if (!IsImmediate(shift, Type::U32)) continue;
				const auto shifted_mask = mask.U32() >> (shift.U32() & 31u);
				if (shifted_mask == 0u) {
					Replace(inst, Value(0u));
					return;
				}
				if (!HasOneUse(*shifted) || shift.U32() >= 32u) continue;
				const auto masked = worklist.Prepend(block, instruction, ValueOpcode::BitwiseAnd32,
				    {Arg(*shifted, 0), Value(shifted_mask)});
				Replace(inst, Value(&*worklist.Prepend(block, instruction, ValueOpcode::ShiftLeftLogical32,
				    {Value(&*masked), shift})));
				return;
			}
			return;
		case ValueOpcode::BitwiseAnd64:
			if (!FoldBinary(inst, [](uint64_t a, uint64_t b) { return a & b; })) {
				ReplaceBinaryIdentity(inst, Type::U64, UINT64_MAX);
			}
			return;
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
			if (!FoldBinary(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
				    return opcode == ValueOpcode::BitwiseOr32 ? a | b : a ^ b;
			    }) && !ReplaceBinaryIdentity(inst, Type::U32, 0u) &&
			    IsComplementPair(inst, ValueOpcode::BitwiseNot32)) {
				Replace(inst, Value(UINT32_MAX));
			}
			return;
		case ValueOpcode::BitwiseNot32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				Replace(inst, Value(~value.U32()));
			} else if (auto* producer = value.TryInstruction();
			           producer != nullptr && producer->GetOpcode() == ValueOpcode::BitwiseNot32) {
				Replace(inst, producer->Arg(0));
			}
			return;
		}
		case ValueOpcode::BitReverse32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				uint32_t source = value.U32(), reversed = 0;
				for (uint32_t bit = 0; bit < 32u; ++bit, source >>= 1u) {
					reversed = (reversed << 1u) | (source & 1u);
				}
				Replace(inst, Value(reversed));
			}
			return;
		}
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindILsb32: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U32)) {
				const auto bits = value.U32();
				Replace(inst, Value(static_cast<uint32_t>(
				    inst.GetOpcode() == ValueOpcode::BitCount32 ? std::popcount(bits)
				    : bits == 0u                                ? -1
				                                                : std::countr_zero(bits))));
			}
			return;
		}
		case ValueOpcode::SMin32:
		case ValueOpcode::SMax32:
			FoldBinary(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
				const auto lhs = std::bit_cast<int32_t>(a);
				const auto rhs = std::bit_cast<int32_t>(b);
				const bool take_a = opcode == ValueOpcode::SMin32 ? lhs < rhs : lhs > rhs;
				return take_a ? a : b;
			});
			return;
		case ValueOpcode::UMin32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return std::min(a, b); })) {
				ReplaceBinaryIdentity(inst, Type::U32, ~0u);
			}
			return;
		case ValueOpcode::UMax32:
			FoldBinary(inst, [](uint32_t a, uint32_t b) { return std::max(a, b); });
			return;
		case ValueOpcode::IEqual32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a == b; })) {
				FoldZeroComparison(block, instruction, worklist);
				if (inst.GetOpcode() == ValueOpcode::IEqual32 && aggressive) {
					FoldBoundedU32Compare(inst);
				}
			}
			return;
		case ValueOpcode::INotEqual32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a != b; })) {
				FoldZeroComparison(block, instruction, worklist);
				if (inst.GetOpcode() == ValueOpcode::INotEqual32 && aggressive) {
					FoldBoundedU32Compare(inst);
				}
			}
			return;
		case ValueOpcode::ULessThan32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a < b; }) && aggressive)
				FoldBoundedU32Compare(inst);
			return;
		case ValueOpcode::ULessThanEqual32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a <= b; }) && aggressive)
				FoldBoundedU32Compare(inst);
			return;
		case ValueOpcode::UGreaterThan32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a > b; }) && aggressive)
				FoldBoundedU32Compare(inst);
			return;
		case ValueOpcode::UGreaterThanEqual32:
			if (!FoldBinary(inst, [](uint32_t a, uint32_t b) { return a >= b; }) && aggressive)
				FoldBoundedU32Compare(inst);
			return;
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
			FoldBinary(inst, [opcode = inst.GetOpcode()](uint32_t a, uint32_t b) {
				const auto lhs = std::bit_cast<int32_t>(a);
				const auto rhs = std::bit_cast<int32_t>(b);
				switch (opcode) {
					case ValueOpcode::SLessThan32: return lhs < rhs;
					case ValueOpcode::SLessThanEqual32: return lhs <= rhs;
					case ValueOpcode::SGreaterThan32: return lhs > rhs;
					default: return lhs >= rhs;
				}
			});
			return;
		case ValueOpcode::IEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a == b; });
			return;
		case ValueOpcode::INotEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a != b; });
			return;
		case ValueOpcode::ULessThan64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a < b; });
			return;
		case ValueOpcode::UGreaterThan64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a > b; });
			return;
		case ValueOpcode::ULessThanEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a <= b; });
			return;
		case ValueOpcode::UGreaterThanEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) { return a >= b; });
			return;
		case ValueOpcode::SLessThanEqual64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) {
				return std::bit_cast<int64_t>(a) <= std::bit_cast<int64_t>(b);
			});
			return;
		case ValueOpcode::SLessThan64:
			FoldBinary(inst, [](uint64_t a, uint64_t b) {
				return std::bit_cast<int64_t>(a) < std::bit_cast<int64_t>(b);
			});
			return;
		case ValueOpcode::LogicalAnd:
			if (!FoldBinary(inst, [](bool a, bool b) { return a && b; })) {
				const auto lhs = Arg(inst, 0);
				const auto rhs = Arg(inst, 1);
				const auto simplify = [&](Value assumption, Value expression) {
					const auto* test = expression.TryInstruction();
					if (test == nullptr) return false;
					const auto replace = [&](Value value) {
						inst.SetArg(assumption == lhs ? 1u : 0u, value);
						return true;
					};
					const auto negate = [&](Value value) {
						return Value(&*worklist.Prepend(block, instruction, ValueOpcode::LogicalNot, {value}));
					};
					const bool inverse = test->GetOpcode() == ValueOpcode::LogicalNot;
					if (inverse) {
						test = Arg(*test, 0).TryInstruction();
						if (test == nullptr) return false;
						if (test->GetOpcode() == ValueOpcode::LogicalAnd && HasOneUse(*test))
							for (uint32_t arg = 0; arg < 2u; ++arg)
								if (Arg(*test, arg) == assumption)
									return replace(negate(Arg(*test, arg ^ 1u)));
					}
					if (test->GetOpcode() == ValueOpcode::IEqual32 ||
					    test->GetOpcode() == ValueOpcode::INotEqual32) {
						for (uint32_t arg = 0; arg < 2u; ++arg) {
							if (Arg(*test, arg) != Value(0u)) continue;
							const auto original = Arg(*test, arg ^ 1u);
							const auto active = ResolveActiveU32(original, assumption);
							if (!active.IsEmpty() && active != original) {
								const auto comparison = Value(&*worklist.Prepend(block, instruction,
								    test->GetOpcode(), {active, Value(0u)}));
								return replace(inverse ? negate(comparison) : comparison);
							}
						}
					}
					if (inverse || test->GetOpcode() != ValueOpcode::LogicalOr) return false;
					for (uint32_t arg = 0; arg < 2u; ++arg) {
						const auto* negation = Arg(*test, arg).TryInstruction();
						if (negation != nullptr && negation->GetOpcode() == ValueOpcode::LogicalNot &&
						    Arg(*negation, 0) == assumption) return replace(Arg(*test, arg ^ 1u));
					}
					return false;
				};
				if (simplify(lhs, rhs) || simplify(rhs, lhs)) return;
				if (IsImmediate(lhs, Type::U1)) {
					Replace(inst, lhs.U1() ? rhs : lhs);
				} else if (IsImmediate(rhs, Type::U1)) {
					Replace(inst, rhs.U1() ? lhs : rhs);
				}
			}
			return;
		case ValueOpcode::LogicalOr:
			if (!FoldBinary(inst, [](bool a, bool b) { return a || b; })) {
				const auto lhs = Arg(inst, 0);
				const auto rhs = Arg(inst, 1);
				if (IsImmediate(lhs, Type::U1)) {
					Replace(inst, lhs.U1() ? lhs : rhs);
				} else if (IsImmediate(rhs, Type::U1)) {
					Replace(inst, rhs.U1() ? rhs : lhs);
				} else if (IsComplementPair(inst, ValueOpcode::LogicalNot)) {
					Replace(inst, Value(true));
				} else {
					const auto* left = lhs.TryInstruction();
					const auto* right = rhs.TryInstruction();
					if (left == nullptr || right == nullptr ||
					    left->GetOpcode() != ValueOpcode::LogicalAnd ||
					    right->GetOpcode() != ValueOpcode::LogicalAnd ||
					    (!HasOneUse(*left) && !HasOneUse(*right))) return;
					// Factor a shared execution predicate without retaining more operations.
					for (uint32_t a = 0; a < 2u; ++a) {
						for (uint32_t b = 0; b < 2u; ++b) {
							const auto common = Arg(*left, a);
							if (common != Arg(*right, b)) continue;
							const auto either = Value(&*worklist.Prepend(block, instruction,
							    ValueOpcode::LogicalOr, {Arg(*left, a ^ 1u), Arg(*right, b ^ 1u)}));
							Replace(inst, Value(&*worklist.Prepend(block, instruction,
							    ValueOpcode::LogicalAnd, {common, either})));
							return;
						}
					}
				}
			}
			return;
		case ValueOpcode::LogicalXor:
			if (!FoldBinary(inst, [](bool a, bool b) { return a != b; })) {
				const auto lhs = Arg(inst, 0);
				const auto rhs = Arg(inst, 1);
				if (IsImmediate(lhs, Type::U1) && !lhs.U1()) {
					Replace(inst, rhs);
				} else if (IsImmediate(rhs, Type::U1) && !rhs.U1()) {
					Replace(inst, lhs);
				}
			}
			return;
		case ValueOpcode::ConditionRef: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U1)) Replace(inst, value);
			return;
		}
		case ValueOpcode::LogicalNot: {
			const auto value = Arg(inst, 0);
			if (IsImmediate(value, Type::U1)) {
				Replace(inst, Value(!value.U1()));
			} else if (auto* producer = value.TryInstruction();
			           producer != nullptr && producer->GetOpcode() == ValueOpcode::LogicalNot) {
				Replace(inst, producer->Arg(0));
			}
			return;
		}
		default: return;
	}
}

// Full and empty masks have a constant predicate in either wave size. In wave32,
// ballot projection also survives scalar EXEC operations and loop-carried mask Phis.
class LaneMaskProjection {
public:
	explicit LaneMaskProjection(FoldWorklist& worklist): m_worklist(worklist) {}

	void Fold(Inst& inst, uint32_t wave_size) {
		if (inst.GetOpcode() != ValueOpcode::INotEqual32 || !Immediate(Arg(inst, 1), 0u)) return;
		const auto* bit = Arg(inst, 0).TryInstruction();
		if (bit == nullptr || bit->GetOpcode() != ValueOpcode::BitwiseAnd32 ||
		    !Immediate(Arg(*bit, 1), 1u)) return;
		const auto* shift = Arg(*bit, 0).TryInstruction();
		if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftRightLogical32) return;
		const auto* index = Arg(*shift, 1).TryInstruction();
		if (index == nullptr || index->GetOpcode() != ValueOpcode::BitwiseAnd32 ||
		    !Immediate(Arg(*index, 1), 31u)) return;
		const auto* lane = Arg(*index, 0).TryInstruction();
		if (lane == nullptr || lane->GetOpcode() != ValueOpcode::LaneId) return;
		const auto mask = Arg(*shift, 0);
		if (Immediate(mask, 0u) || Immediate(mask, UINT32_MAX)) {
			Replace(inst, Value(mask.U32() != 0u));
			return;
		}
		if (wave_size != 32u) return;
		m_visited.clear();
		m_grounded = false;
		if (CanProject(mask) && m_grounded) Replace(inst, Project(mask));
	}

private:
	static bool Immediate(Value value, uint32_t expected) {
		return IsImmediate(value, Type::U32) && value.U32() == expected;
	}

	static Value BallotPredicate(const Inst& inst) {
		if (inst.GetOpcode() != ValueOpcode::CompositeExtractU32x4 ||
		    !Immediate(Arg(inst, 1), 0u)) return {};
		const auto* source = Arg(inst, 0).TryInstruction();
		if (source == nullptr) return {};
		// Helper lanes keep the per-lane EXEC, so the live mask projects to true.
		if (source->GetOpcode() == ValueOpcode::LiveMask) return Value(true);
		return source->GetOpcode() == ValueOpcode::Ballot ? Arg(*source, 0) : Value {};
	}

	bool CanProject(Value value) {
		if (value.IsImmediate()) {
			const bool supported = Immediate(value, 0u) || Immediate(value, UINT32_MAX);
			m_grounded |= supported;
			return supported;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (m_values.contains(inst) || !BallotPredicate(*inst).IsEmpty()) {
			m_grounded = true;
			return true;
		}
		if (!m_visited.insert(inst).second) return true;
		const auto op = inst->GetOpcode();
		if (op != ValueOpcode::BitwiseAnd32 && op != ValueOpcode::BitwiseOr32 &&
		    op != ValueOpcode::BitwiseNot32 && op != ValueOpcode::SelectU32 && op != ValueOpcode::Phi)
			return false;
		for (size_t arg = op == ValueOpcode::SelectU32 ? 1u : 0u; arg < inst->NumArgs(); ++arg) {
			if (!CanProject(Arg(*inst, arg))) return false;
		}
		return inst->NumArgs() != 0;
	}

	Value Project(Value value) {
		if (value.IsImmediate()) return Value(value.U32() != 0u);
		auto* source = value.TryInstruction();
		if (const auto found = m_values.find(source); found != m_values.end()) return found->second.Resolve();
		if (const auto predicate = BallotPredicate(*source); !predicate.IsEmpty()) return predicate;
		auto* block = source->Parent();
		const auto where = m_worklist.locations.at(source).instruction;
		const auto op = source->GetOpcode() == ValueOpcode::BitwiseAnd32 ? ValueOpcode::LogicalAnd
		              : source->GetOpcode() == ValueOpcode::BitwiseOr32 ? ValueOpcode::LogicalOr
		              : source->GetOpcode() == ValueOpcode::BitwiseNot32 ? ValueOpcode::LogicalNot
		              : source->GetOpcode() == ValueOpcode::SelectU32 ? ValueOpcode::SelectU1
		                                                               : ValueOpcode::Phi;
		auto result = op == ValueOpcode::Phi ? m_worklist.Prepend(*block, where, op)
		            : op == ValueOpcode::LogicalNot
		                ? m_worklist.Prepend(*block, where, op, {Value(false)})
		            : op == ValueOpcode::SelectU1
		                ? m_worklist.Prepend(*block, where, op, {Arg(*source, 0), Value(false), Value(false)})
		                : m_worklist.Prepend(*block, where, op, {Value(false), Value(false)});
		m_values.emplace(source, Value(&*result));
		if (op == ValueOpcode::Phi) result->SetFlags(Type::U1);
		for (size_t arg = op == ValueOpcode::SelectU1 ? 1u : 0u; arg < source->NumArgs(); ++arg) {
			const auto projected = Project(Arg(*source, arg));
			if (op == ValueOpcode::Phi) result->AddPhiOperand(source->PhiBlock(arg), projected);
			else result->SetArg(arg, projected);
		}
		FoldInstruction(*block, result, m_worklist, false);
		return Value(&*result).Resolve();
	}

	FoldWorklist& m_worklist;
	std::unordered_set<const Inst*> m_visited;
	std::unordered_map<const Inst*, Value> m_values;
	bool m_grounded = false;
};

class DominanceInfo {
public:
	explicit DominanceInfo(const BlockList& blocks, bool build_inst_seq = true) {
		if (blocks.empty()) return;
		std::unordered_set<const Block*> seen;
		std::vector<const Block*>        postorder;
		const auto                       dfs = [&](auto& self, const Block* b) -> void {
			if (!seen.insert(b).second) return;
			for (const auto* succ: b->ImmSuccessors()) self(self, succ);
			postorder.push_back(b);
		};
		dfs(dfs, blocks.front());
		m_rpo.assign(postorder.rbegin(), postorder.rend());
		m_rpo_index.reserve(m_rpo.size());
		for (size_t i = 0; i < m_rpo.size(); ++i) m_rpo_index.emplace(m_rpo[i], static_cast<int>(i));
		m_idom.assign(m_rpo.size(), -1);
		m_idom[0] = 0;
		const auto intersect = [&](int a, int b) {
			while (a != b) {
				while (a > b) a = m_idom[a];
				while (b > a) b = m_idom[b];
			}
			return a;
		};
		bool changed = true;
		while (changed) {
			changed = false;
			for (size_t i = 1; i < m_rpo.size(); ++i) {
				int new_idom = -1;
				for (const auto* pred: m_rpo[i]->ImmPredecessors()) {
					const auto it = m_rpo_index.find(pred);
					if (it == m_rpo_index.end() || m_idom[it->second] == -1) continue;
					new_idom = new_idom == -1 ? it->second : intersect(it->second, new_idom);
				}
				if (new_idom != -1 && m_idom[i] != new_idom) {
					m_idom[i] = new_idom;
					changed   = true;
				}
			}
		}
		if (build_inst_seq) {
			size_t total = 0;
			for (const auto* block: blocks) total += block->Instructions().size();
			m_inst_seq.reserve(total);
			for (const auto* block: blocks) {
				uint32_t seq = 0;
				for (const auto& inst: *block) m_inst_seq.emplace(&inst, seq++);
			}
		}
	}

	const std::vector<const Block*>& Rpo() const { return m_rpo; }

	bool IsReachable(const Block* block) const { return m_rpo_index.contains(block); }

	bool StrictlyDominatesBlock(const Block* a, const Block* b) const {
		if (a == nullptr || b == nullptr || a == b) return false;
		const auto ita = m_rpo_index.find(a);
		const auto itb = m_rpo_index.find(b);
		if (ita == m_rpo_index.end() || itb == m_rpo_index.end()) return false;
		const int ia = ita->second;
		int       curr = itb->second;
		while (curr > ia && curr > 0) curr = m_idom[curr];
		return curr == ia;
	}

	bool Dominates(const Inst* a, const Inst* b) const {
		if (a == nullptr || b == nullptr) return false;
		const auto* ba = a->Parent();
		const auto* bb = b->Parent();
		if (ba == bb) {
			const auto sa = m_inst_seq.find(a);
			const auto sb = m_inst_seq.find(b);
			return sa != m_inst_seq.end() && sb != m_inst_seq.end() && sa->second <= sb->second;
		}
		return StrictlyDominatesBlock(ba, bb);
	}

private:
	std::vector<const Block*>             m_rpo;
	std::unordered_map<const Block*, int> m_rpo_index;
	std::vector<int>                      m_idom;
	std::unordered_map<const Inst*, uint32_t> m_inst_seq;
};

bool IsSelectOpcode(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32 || op == ValueOpcode::SelectF64;
}

bool IsPureLaneLocal(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU16F16:
		case ValueOpcode::BitCastF16U16:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::BitCastU64F64:
		case ValueOpcode::BitCastF64U64:
		case ValueOpcode::ConvertU16U32:
		case ValueOpcode::ConvertU32U16:
		case ValueOpcode::ConvertU8U32:
		case ValueOpcode::ConvertU32U8:
		case ValueOpcode::ConvertF32F16:
		case ValueOpcode::ConvertF16F32:
		case ValueOpcode::ConvertS32F32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32S32:
		case ValueOpcode::ConvertF64S32:
		case ValueOpcode::ConvertF32F64:
		case ValueOpcode::ConvertF64F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::ConvertF64U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeConstructU32x3:
		case ValueOpcode::CompositeConstructF32x2:
		case ValueOpcode::CompositeConstructU32x4:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::PackHalf2x16:
		case ValueOpcode::PackSnorm2x16:
		case ValueOpcode::PackUnorm2x16:
		case ValueOpcode::PackFloat2x16Rtz:
		case ValueOpcode::FPAbs32:
		case ValueOpcode::FPNeg32:
		case ValueOpcode::FPSaturate32:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::SelectF64:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::IAbs32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
		case ValueOpcode::SMinTri32:
		case ValueOpcode::UMinTri32:
		case ValueOpcode::SMaxTri32:
		case ValueOpcode::UMaxTri32:
		case ValueOpcode::SMedTri32:
		case ValueOpcode::UMedTri32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::SLessThanEqual64:
		case ValueOpcode::ULessThanEqual64:
		case ValueOpcode::UGreaterThanEqual64:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::ConditionRef:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdEqual64:
		case ValueOpcode::FPOrdLessThanEqual64:
		case ValueOpcode::FPOrdGreaterThanEqual64:
		case ValueOpcode::FPOrdNotEqual64:
		case ValueOpcode::FPOrdLessThan64:
		case ValueOpcode::FPOrdGreaterThan64:
		case ValueOpcode::FPUnordEqual64:
		case ValueOpcode::FPUnordNotEqual64:
		case ValueOpcode::FPUnordLessThan64:
		case ValueOpcode::FPUnordGreaterThan64:
		case ValueOpcode::FPUnordLessThanEqual64:
		case ValueOpcode::FPUnordGreaterThanEqual64:
		case ValueOpcode::FPUnordEqual32:
		case ValueOpcode::FPOrdNotEqual32:
		case ValueOpcode::FPUnordNotEqual32:
		case ValueOpcode::FPOrdLessThan32:
		case ValueOpcode::FPUnordLessThan32:
		case ValueOpcode::FPOrdGreaterThan32:
		case ValueOpcode::FPUnordGreaterThan32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPUnordLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPUnordGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPIsNan64:
		case ValueOpcode::FPCmpClass32:
		case ValueOpcode::FPCmpClass16:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPAdd64:
		case ValueOpcode::FPMul64:
		case ValueOpcode::FPFma64:
		case ValueOpcode::FPRecip64:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::FPMin64:
		case ValueOpcode::FPMax64:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPRecipSqrt32:
		case ValueOpcode::FPSqrt:
		case ValueOpcode::FPSin:
		case ValueOpcode::FPCos:
		case ValueOpcode::FPExp2:
		case ValueOpcode::FPLog2:
		case ValueOpcode::FPLdexp:
		case ValueOpcode::FPRoundEven32:
		case ValueOpcode::FPFloor32:
		case ValueOpcode::FPCeil32:
		case ValueOpcode::FPTrunc32:
		case ValueOpcode::FPFloor64:
		case ValueOpcode::FPCeil64:
		case ValueOpcode::FPTrunc64:
		case ValueOpcode::FPFract32:
		case ValueOpcode::FPFract64:
		case ValueOpcode::MakeImageAddress:
		case ValueOpcode::Bvh8Extract: return true;
		default: return false;
	}
}

int PredicatedActiveOperand(const Inst& inst) {
	switch (inst.GetOpcode()) {
		case ValueOpcode::LoadAddressU8:
		case ValueOpcode::LoadAddressU16:
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::LoadDeviceAddressU32:
		case ValueOpcode::StoreAddressU8:
		case ValueOpcode::StoreAddressU16:
		case ValueOpcode::StoreAddressU32:
		case ValueOpcode::LoadBufferU8:
		case ValueOpcode::LoadBufferU16:
		case ValueOpcode::LoadBufferU32:
		case ValueOpcode::LoadBufferU32x2:
		case ValueOpcode::LoadBufferU32x3:
		case ValueOpcode::LoadBufferU32x4:
		case ValueOpcode::StoreBufferU8:
		case ValueOpcode::StoreBufferU16:
		case ValueOpcode::StoreBufferU32:
		case ValueOpcode::StoreBufferU32x2:
		case ValueOpcode::StoreBufferU32x3:
		case ValueOpcode::StoreBufferU32x4:
		case ValueOpcode::BufferAtomicSwap32:
		case ValueOpcode::BufferAtomicCmpSwap32:
		case ValueOpcode::BufferAtomicSwap64:
		case ValueOpcode::BufferAtomicIAdd32:
		case ValueOpcode::BufferAtomicIAdd64:
		case ValueOpcode::BufferAtomicISub32:
		case ValueOpcode::BufferAtomicSMin32:
		case ValueOpcode::BufferAtomicUMin32:
		case ValueOpcode::BufferAtomicUMin64:
		case ValueOpcode::BufferAtomicSMax32:
		case ValueOpcode::BufferAtomicUMax32:
		case ValueOpcode::BufferAtomicAnd32:
		case ValueOpcode::BufferAtomicAnd64:
		case ValueOpcode::BufferAtomicOr32:
		case ValueOpcode::BufferAtomicOr64:
		case ValueOpcode::BufferAtomicXor32:
		case ValueOpcode::BufferAtomicFMin32:
		case ValueOpcode::BufferAtomicFMax32:
		case ValueOpcode::LoadSharedU8:
		case ValueOpcode::LoadSharedU16:
		case ValueOpcode::LoadSharedU32:
		case ValueOpcode::LoadSharedU32x2:
		case ValueOpcode::LoadSharedU32x3:
		case ValueOpcode::LoadSharedU32x4:
		case ValueOpcode::WriteSharedU8:
		case ValueOpcode::WriteSharedU16:
		case ValueOpcode::WriteSharedU32:
		case ValueOpcode::WriteSharedU32x2:
		case ValueOpcode::WriteSharedU32x3:
		case ValueOpcode::WriteSharedU32x4:
		case ValueOpcode::SharedAtomicFMin32:
		case ValueOpcode::SharedAtomicFMax32:
		case ValueOpcode::SharedAtomicSwap32:
		case ValueOpcode::SharedAtomicIAdd32:
		case ValueOpcode::SharedAtomicIAdd64:
		case ValueOpcode::SharedAtomicISub32:
		case ValueOpcode::SharedAtomicInc32:
		case ValueOpcode::SharedAtomicDec32:
		case ValueOpcode::SharedAtomicSMin32:
		case ValueOpcode::SharedAtomicUMin32:
		case ValueOpcode::SharedAtomicSMax32:
		case ValueOpcode::SharedAtomicUMax32:
		case ValueOpcode::SharedAtomicUMax64:
		case ValueOpcode::SharedAtomicCmpSwap32:
		case ValueOpcode::SharedAtomicCmpSwap64:
		case ValueOpcode::SharedAtomicAnd32:
		case ValueOpcode::SharedAtomicOr32:
		case ValueOpcode::SharedAtomicOr64:
		case ValueOpcode::SharedAtomicXor32:
		case ValueOpcode::SharedAtomicMaskedOr32:
		case ValueOpcode::ImageRead:
		case ValueOpcode::ImageWrite:
		case ValueOpcode::ImageGatherRaw:
		case ValueOpcode::ImageAtomicCompareSwap32:
		case ValueOpcode::ImageAtomicSwap32:
		case ValueOpcode::ImageAtomicSwap64:
		case ValueOpcode::ImageAtomicIAdd32:
		case ValueOpcode::ImageAtomicIAdd64:
		case ValueOpcode::ImageAtomicSMin32:
		case ValueOpcode::ImageAtomicUMin32:
		case ValueOpcode::ImageAtomicUMin64:
		case ValueOpcode::ImageAtomicSMax32:
		case ValueOpcode::ImageAtomicUMax32:
		case ValueOpcode::ImageAtomicUMax64:
		case ValueOpcode::ImageAtomicAnd32:
		case ValueOpcode::ImageAtomicAnd64:
		case ValueOpcode::ImageAtomicOr32:
		case ValueOpcode::ImageAtomicOr64:
		case ValueOpcode::ImageAtomicXor32:
		case ValueOpcode::ImageAtomicXor64:
		case ValueOpcode::ImageAtomicFMin32:
		case ValueOpcode::ImageAtomicFMax32:
		case ValueOpcode::SetAttribute:
		case ValueOpcode::SetTessellationAttribute:
		case ValueOpcode::GetTessellationAttribute:
		case ValueOpcode::ImageSampleRaw:
		case ValueOpcode::BvhIntersect:
		case ValueOpcode::Bvh8Intersect:
		case ValueOpcode::BvhStack:
			return static_cast<int>(inst.NumArgs()) - 1;
		default: return -1;
	}
}

bool EliminateGuardedSelects(const BlockList& blocks, FoldWorklist& worklist) {
	if (blocks.empty()) return false;
	const DominanceInfo dom(blocks, true);

	struct DemandState {
		bool                  initialized = false;
		std::vector<BoolFact> facts;
	};
	size_t inst_count = 0;
	for (const auto* block: blocks) inst_count += block->Instructions().size();
	std::unordered_map<const Inst*, uint32_t> inst_ids;
	inst_ids.reserve(inst_count);
	for (const auto* block: blocks) {
		for (const auto& inst: *block) inst_ids.emplace(&inst, static_cast<uint32_t>(inst_ids.size()));
	}
	std::vector<DemandState> demands(inst_ids.size());

	const auto mark_unconstrained = [&](Value v) {
		if (const auto* inst = v.Resolve().TryInstruction(); inst != nullptr) {
			if (const auto it = inst_ids.find(inst); it != inst_ids.end()) {
				demands[it->second] = {true, {}};
			}
		}
	};

	for (const auto* block: blocks) {
		mark_unconstrained(block->condition);
		mark_unconstrained(block->indirect_target);
		if (!dom.IsReachable(block)) {
			for (const auto& inst: *block) {
				for (size_t arg = 0; arg < inst.NumArgs(); ++arg) mark_unconstrained(inst.Arg(arg));
			}
		}
	}

	const auto filter_dominating = [&](std::span<const BoolFact> facts, const Inst* target,
	                                   std::vector<BoolFact>& out) {
		out.clear();
		for (const auto& fact: facts) {
			const auto* fact_inst = fact.value.TryInstruction();
			if (fact_inst == nullptr || dom.Dominates(fact_inst, target)) out.push_back(fact);
		}
	};
	const auto filter_strict_block_dom = [&](std::span<const BoolFact> facts, const Block* phi_block,
	                                         std::vector<BoolFact>& out) {
		out.clear();
		for (const auto& fact: facts) {
			const auto* fact_inst = fact.value.TryInstruction();
			if (fact_inst == nullptr || dom.StrictlyDominatesBlock(fact_inst->Parent(), phi_block)) {
				out.push_back(fact);
			}
		}
	};
	const auto merge_demand = [&](uint32_t target_id, std::span<const BoolFact> dom_facts) -> bool {
		auto& state = demands[target_id];
		if (!state.initialized) {
			state.initialized = true;
			state.facts.assign(dom_facts.begin(), dom_facts.end());
			return true;
		}
		const size_t prev_size = state.facts.size();
		std::erase_if(state.facts, [&](const BoolFact& existing) {
			return std::ranges::find(dom_facts, existing) == dom_facts.end();
		});
		return state.facts.size() != prev_size;
	};

	std::vector<BoolFact> dom_facts;
	const auto            resolve_select_chain = [&](Value operand, std::span<const BoolFact> use_facts) -> Value {
		if (use_facts.empty()) return operand.Resolve();
		for (size_t depth = 0; depth < 16u; ++depth) {
			operand      = operand.Resolve();
			auto* target = operand.TryInstruction();
			if (target == nullptr) return operand;
			filter_dominating(use_facts, target, dom_facts);
			if (IsSelectOpcode(target->GetOpcode())) {
				const auto cond = Arg(*target, 0);
				if (ImpliesBoolValue(dom_facts, cond, true)) {
					operand = Arg(*target, 1);
					continue;
				}
				if (ImpliesBoolValue(dom_facts, cond, false)) {
					operand = Arg(*target, 2);
					continue;
				}
			}
			return operand;
		}
		return operand.Resolve();
	};

	const auto& rpo = dom.Rpo();
	std::vector<int32_t> inst_block_rpo(inst_ids.size(), -1);
	for (size_t bi = 0; bi < rpo.size(); ++bi) {
		for (const auto& inst: *rpo[bi]) {
			inst_block_rpo[inst_ids.at(&inst)] = static_cast<int32_t>(bi);
		}
	}
	std::vector<uint8_t>  curr_dirty(rpo.size(), 1u);
	std::vector<uint8_t>  next_dirty(rpo.size(), 0u);
	std::vector<BoolFact> out_facts;
	std::vector<BoolFact> phi_facts;
	std::vector<BoolFact> active_facts;
	std::vector<BoolFact> true_facts;
	std::vector<BoolFact> false_facts;

	bool converged = false;
	for (size_t iter = 0; iter < 12u; ++iter) {
		bool any_next_dirty = false;
		for (size_t bi = rpo.size(); bi-- > 0;) {
			if (!curr_dirty[bi]) continue;
			curr_dirty[bi]    = 0u;
			const auto* block = rpo[bi];
			for (auto iit = block->Instructions().rbegin(); iit != block->Instructions().rend(); ++iit) {
				const auto& user = *iit;
				const auto  op   = user.GetOpcode();
				if (op == ValueOpcode::Identity || op == ValueOpcode::Void) continue;
				if (!user.HasUses() && !user.MayHaveSideEffects()) continue;

				const bool phi_user       = op == ValueOpcode::Phi;
				const auto demand_operand = [&](Value operand, std::span<const BoolFact> use_facts) {
					if (use_facts.empty()) {
						operand = operand.Resolve();
						if (const auto* target = operand.TryInstruction(); target != nullptr) {
							if (const auto it = inst_ids.find(target); it != inst_ids.end()) {
								if (merge_demand(it->second, {})) {
									const int32_t tbi = inst_block_rpo[it->second];
									if (tbi >= 0) {
										if (static_cast<size_t>(tbi) < bi) {
											curr_dirty[static_cast<size_t>(tbi)] = 1u;
										} else if (static_cast<size_t>(tbi) > bi || phi_user) {
											next_dirty[static_cast<size_t>(tbi)] = 1u;
											any_next_dirty                       = true;
										}
									}
								}
							}
						}
						return;
					}
					operand = resolve_select_chain(operand, use_facts);
					if (const auto* target = operand.TryInstruction(); target != nullptr) {
						if (const auto it = inst_ids.find(target); it != inst_ids.end()) {
							filter_dominating(use_facts, target, dom_facts);
							if (merge_demand(it->second, dom_facts)) {
								const int32_t tbi = inst_block_rpo[it->second];
								if (tbi >= 0) {
									if (static_cast<size_t>(tbi) < bi) {
										curr_dirty[static_cast<size_t>(tbi)] = 1u;
									} else if (static_cast<size_t>(tbi) > bi || phi_user) {
										next_dirty[static_cast<size_t>(tbi)] = 1u;
										any_next_dirty                       = true;
									}
								}
							}
						}
					}
				};

				if (phi_user) {
					const auto it = inst_ids.find(&user);
					if (it == inst_ids.end() || !demands[it->second].initialized) continue;
					filter_strict_block_dom(demands[it->second].facts, user.Parent(), phi_facts);
					for (size_t k = 0; k < user.NumArgs(); ++k) {
						demand_operand(Arg(user, k), phi_facts);
					}
					continue;
				}

				if (!user.MayHaveSideEffects() && IsPureLaneLocal(op)) {
					const auto it = inst_ids.find(&user);
					if (it == inst_ids.end() || !demands[it->second].initialized) continue;
					out_facts = demands[it->second].facts;
				} else {
					out_facts.clear();
				}

				const int active_arg = PredicatedActiveOperand(user);
				if (active_arg >= 0) {
					demand_operand(Arg(user, static_cast<size_t>(active_arg)), out_facts);
					active_facts = out_facts;
					CollectBoolFacts(Arg(user, static_cast<size_t>(active_arg)), true, active_facts);
					for (size_t k = 0; k < user.NumArgs(); ++k) {
						if (static_cast<int>(k) != active_arg) {
							demand_operand(Arg(user, k), active_facts);
						}
					}
				} else if (IsSelectOpcode(op)) {
					demand_operand(Arg(user, 0u), out_facts);
					const auto cond = Arg(user, 0);
					true_facts = out_facts;
					CollectBoolFacts(cond, true, true_facts);
					demand_operand(Arg(user, 1u), true_facts);
					false_facts = out_facts;
					CollectBoolFacts(cond, false, false_facts);
					demand_operand(Arg(user, 2u), false_facts);
				} else {
					for (size_t k = 0; k < user.NumArgs(); ++k) {
						demand_operand(Arg(user, k), out_facts);
					}
				}
			}
		}
		if (!any_next_dirty) {
			converged = true;
			break;
		}
		curr_dirty.swap(next_dirty);
		std::fill(next_dirty.begin(), next_dirty.end(), 0u);
	}
	if (!converged) return false;

	bool       changed         = false;
	const auto rewrite_operand = [&](Inst& user, size_t arg_index, std::span<const BoolFact> use_facts) {
		const Value orig       = Arg(user, arg_index);
		const Value simplified = resolve_select_chain(orig, use_facts);
		if (simplified != orig) {
			if (auto* old_inst = orig.TryInstruction()) worklist.QueueUsers(old_inst->Uses());
			user.SetArg(arg_index, simplified);
			if (const auto it = worklist.locations.find(&user); it != worklist.locations.end()) {
				worklist.Queue(it->second);
			}
			changed = true;
		}
	};

	for (auto bit = rpo.rbegin(); bit != rpo.rend(); ++bit) {
		auto* block = const_cast<Block*>(*bit);
		for (auto iit = block->Instructions().rbegin(); iit != block->Instructions().rend(); ++iit) {
			auto&      user = *iit;
			const auto op   = user.GetOpcode();
			if (op == ValueOpcode::Identity || op == ValueOpcode::Void) continue;
			if (!user.HasUses() && !user.MayHaveSideEffects()) continue;

			if (op == ValueOpcode::Phi) {
				const auto it = inst_ids.find(&user);
				if (it == inst_ids.end() || !demands[it->second].initialized) continue;
				filter_strict_block_dom(demands[it->second].facts, user.Parent(), phi_facts);
				for (size_t k = 0; k < user.NumArgs(); ++k) {
					rewrite_operand(user, k, phi_facts);
				}
				continue;
			}

			if (!user.MayHaveSideEffects() && IsPureLaneLocal(op)) {
				const auto it = inst_ids.find(&user);
				if (it == inst_ids.end() || !demands[it->second].initialized) continue;
				out_facts = demands[it->second].facts;
			} else {
				out_facts.clear();
			}

			const int active_arg = PredicatedActiveOperand(user);
			if (active_arg >= 0) {
				rewrite_operand(user, static_cast<size_t>(active_arg), out_facts);
				active_facts = out_facts;
				CollectBoolFacts(Arg(user, static_cast<size_t>(active_arg)), true, active_facts);
				for (size_t k = 0; k < user.NumArgs(); ++k) {
					if (static_cast<int>(k) != active_arg) {
						rewrite_operand(user, k, active_facts);
					}
				}
			} else if (IsSelectOpcode(op)) {
				rewrite_operand(user, 0u, out_facts);
				const auto cond = Arg(user, 0);
				true_facts = out_facts;
				CollectBoolFacts(cond, true, true_facts);
				rewrite_operand(user, 1u, true_facts);
				false_facts = out_facts;
				CollectBoolFacts(cond, false, false_facts);
				rewrite_operand(user, 2u, false_facts);
			} else {
				for (size_t k = 0; k < user.NumArgs(); ++k) {
					rewrite_operand(user, k, out_facts);
				}
			}
		}
	}
	return changed;
}

bool FoldOptimisticU32Ranges(const BlockList& blocks, FoldWorklist& worklist) {
	if (blocks.empty()) return false;
	const DominanceInfo dom(blocks, false);
	const auto&         rpo = dom.Rpo();

	enum class BoolState : uint8_t { Bottom, False, True, Top };

	struct U32State {
		bool     defined = false;
		U32Range range {};
		uint8_t  changes = 0;

		bool operator==(const U32State& other) const {
			return defined == other.defined && (!defined || range == other.range);
		}
	};

	const auto is_tracked_u32 = [](ValueOpcode op) {
		switch (op) {
			case ValueOpcode::Phi:
			case ValueOpcode::SelectU32:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitFieldUExtract:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::UMin32:
			case ValueOpcode::UMax32:
			case ValueOpcode::ConvertU32U8:
			case ValueOpcode::ConvertU32U16:
			case ValueOpcode::IAdd32:
			case ValueOpcode::IMul32:
			case ValueOpcode::ReadFirstLane:
			case ValueOpcode::ReadLane: return true;
			default: return false;
		}
	};
	const auto is_tracked_u1 = [](ValueOpcode op) {
		switch (op) {
			case ValueOpcode::Phi:
			case ValueOpcode::SelectU1:
			case ValueOpcode::LogicalAnd:
			case ValueOpcode::LogicalOr:
			case ValueOpcode::LogicalNot:
			case ValueOpcode::ConditionRef:
			case ValueOpcode::IEqual32:
			case ValueOpcode::INotEqual32:
			case ValueOpcode::ULessThan32:
			case ValueOpcode::ULessThanEqual32:
			case ValueOpcode::UGreaterThan32:
			case ValueOpcode::UGreaterThanEqual32: return true;
			default: return false;
		}
	};

	bool                has_phi     = false;
	bool                has_u32_cmp = false;
	std::vector<Inst*>  u32_nodes;
	std::vector<Inst*>  u1_nodes;
	for (const auto* block: rpo) {
		for (auto& inst: *const_cast<Block*>(block)) {
			const auto op   = inst.GetOpcode();
			const auto type = inst.GetType();
			if (op == ValueOpcode::Phi && (type == Type::U32 || type == Type::U1)) has_phi = true;
			if (type == Type::U32) {
				if (is_tracked_u32(op)) u32_nodes.push_back(&inst);
			} else if (type == Type::U1) {
				if (op >= ValueOpcode::ULessThan32 && op <= ValueOpcode::UGreaterThanEqual32) {
					has_u32_cmp = true;
				}
				if (op == ValueOpcode::IEqual32 || op == ValueOpcode::INotEqual32) {
					has_u32_cmp = true;
				}
				if (is_tracked_u1(op)) u1_nodes.push_back(&inst);
			}
		}
	}
	if (!has_phi || !has_u32_cmp) return false;

	std::unordered_map<const Inst*, uint32_t> u32_id;
	std::unordered_map<const Inst*, uint32_t> u1_id;
	u32_id.reserve(u32_nodes.size());
	u1_id.reserve(u1_nodes.size());
	for (uint32_t i = 0; i < static_cast<uint32_t>(u32_nodes.size()); ++i) u32_id.emplace(u32_nodes[i], i);
	for (uint32_t i = 0; i < static_cast<uint32_t>(u1_nodes.size()); ++i) u1_id.emplace(u1_nodes[i], i);

	struct ResolvedU32Arg {
		int32_t  node     = -1;
		bool     is_const = false;
		uint32_t value    = 0u;
	};
	struct ResolvedBoolArg {
		int32_t   node     = -1;
		BoolState constant = BoolState::Top;
	};
	struct ActiveNode {
		Inst*       inst      = nullptr;
		ValueOpcode op        = ValueOpcode::Void;
		uint32_t    dense_id  = 0u;
		bool        is_u32    = false;
		bool        same_args = false;
		uint32_t    u32_off   = 0u;
		uint32_t    u32_cnt   = 0u;
		uint32_t    u1_off    = 0u;
		uint32_t    u1_cnt    = 0u;
	};

	std::vector<ResolvedU32Arg>  u32_args;
	std::vector<ResolvedBoolArg> u1_args;
	std::vector<ActiveNode>      active_nodes;
	active_nodes.reserve(u32_nodes.size() + u1_nodes.size());

	const auto resolve_u32_arg = [&](Value value) -> ResolvedU32Arg {
		value = value.Resolve();
		if (IsImmediate(value, Type::U32)) return {-1, true, value.U32()};
		if (const auto* inst = value.TryInstruction(); inst != nullptr) {
			if (const auto it = u32_id.find(inst); it != u32_id.end()) {
				return {static_cast<int32_t>(it->second), false, 0u};
			}
		}
		return {-1, false, 0u};
	};
	const auto resolve_u1_arg = [&](Value value) -> ResolvedBoolArg {
		value = value.Resolve();
		if (IsImmediate(value, Type::U1)) {
			return {-1, value.U1() ? BoolState::True : BoolState::False};
		}
		if (const auto* inst = value.TryInstruction(); inst != nullptr) {
			if (const auto it = u1_id.find(inst); it != u1_id.end()) {
				return {static_cast<int32_t>(it->second), BoolState::Top};
			}
		}
		return {-1, BoolState::Top};
	};

	for (const auto* block: rpo) {
		for (auto& inst: *const_cast<Block*>(block)) {
			const auto op   = inst.GetOpcode();
			const auto type = inst.GetType();
			if (type == Type::U32 && is_tracked_u32(op)) {
				ActiveNode node {
				    .inst     = &inst,
				    .op       = op,
				    .dense_id = u32_id.at(&inst),
				    .is_u32   = true,
				    .u32_off  = static_cast<uint32_t>(u32_args.size()),
				    .u1_off   = static_cast<uint32_t>(u1_args.size()),
				};
				if (op == ValueOpcode::Phi) {
					for (size_t k = 0; k < inst.NumArgs(); ++k) u32_args.push_back(resolve_u32_arg(inst.Arg(k)));
				} else if (op == ValueOpcode::SelectU32) {
					u1_args.push_back(resolve_u1_arg(inst.Arg(0)));
					u32_args.push_back(resolve_u32_arg(inst.Arg(1)));
					u32_args.push_back(resolve_u32_arg(inst.Arg(2)));
				} else if (op == ValueOpcode::BitwiseAnd32 || op == ValueOpcode::BitwiseOr32 ||
				           op == ValueOpcode::UMin32 || op == ValueOpcode::UMax32 ||
				           op == ValueOpcode::IAdd32 || op == ValueOpcode::IMul32) {
					u32_args.push_back(resolve_u32_arg(inst.Arg(0)));
					u32_args.push_back(resolve_u32_arg(inst.Arg(1)));
				} else if (op == ValueOpcode::BitFieldUExtract ||
				           op == ValueOpcode::ShiftRightLogical32 ||
				           op == ValueOpcode::ShiftLeftLogical32 ||
				           op == ValueOpcode::ReadFirstLane || op == ValueOpcode::ReadLane) {
					u32_args.push_back(resolve_u32_arg(inst.Arg(0)));
				}
				node.u32_cnt = static_cast<uint32_t>(u32_args.size()) - node.u32_off;
				node.u1_cnt  = static_cast<uint32_t>(u1_args.size()) - node.u1_off;
				active_nodes.push_back(node);
			} else if (type == Type::U1 && is_tracked_u1(op)) {
				ActiveNode node {
				    .inst     = &inst,
				    .op       = op,
				    .dense_id = u1_id.at(&inst),
				    .is_u32   = false,
				    .u32_off  = static_cast<uint32_t>(u32_args.size()),
				    .u1_off   = static_cast<uint32_t>(u1_args.size()),
				};
				if (op == ValueOpcode::Phi) {
					for (size_t k = 0; k < inst.NumArgs(); ++k) u1_args.push_back(resolve_u1_arg(inst.Arg(k)));
				} else if (op == ValueOpcode::SelectU1) {
					for (size_t k = 0; k < 3u; ++k) u1_args.push_back(resolve_u1_arg(inst.Arg(k)));
				} else if (op == ValueOpcode::LogicalAnd || op == ValueOpcode::LogicalOr) {
					u1_args.push_back(resolve_u1_arg(inst.Arg(0)));
					u1_args.push_back(resolve_u1_arg(inst.Arg(1)));
				} else if (op == ValueOpcode::LogicalNot || op == ValueOpcode::ConditionRef) {
					u1_args.push_back(resolve_u1_arg(inst.Arg(0)));
				} else {
					node.same_args = Arg(inst, 0) == Arg(inst, 1);
					u32_args.push_back(resolve_u32_arg(inst.Arg(0)));
					u32_args.push_back(resolve_u32_arg(inst.Arg(1)));
				}
				node.u32_cnt = static_cast<uint32_t>(u32_args.size()) - node.u32_off;
				node.u1_cnt  = static_cast<uint32_t>(u1_args.size()) - node.u1_off;
				active_nodes.push_back(node);
			}
		}
	}

	std::vector<U32State>  u32_states(u32_nodes.size());
	std::vector<BoolState> bool_states(u1_nodes.size(), BoolState::Bottom);

	const auto eval_u32 = [&](ResolvedU32Arg a) -> U32State {
		if (a.node >= 0) return u32_states[static_cast<size_t>(a.node)];
		return a.is_const ? U32State {true, ConstU32Range(a.value)} : U32State {true, FullU32Range};
	};
	const auto eval_bool = [&](ResolvedBoolArg a) -> BoolState {
		return a.node >= 0 ? bool_states[static_cast<size_t>(a.node)] : a.constant;
	};
	const auto join_bool = [](BoolState a, BoolState b) -> BoolState {
		if (a == BoolState::Bottom) return b;
		if (b == BoolState::Bottom) return a;
		return a == b ? a : BoolState::Top;
	};

	for (size_t iter = 0; iter < 8u; ++iter) {
		bool changed = false;
		for (const auto& node: active_nodes) {
			if (node.is_u32) {
				U32State next;
				switch (node.op) {
					case ValueOpcode::Phi: {
						bool any = false;
						for (uint32_t k = 0; k < node.u32_cnt; ++k) {
							const auto s = eval_u32(u32_args[node.u32_off + k]);
							if (!s.defined) continue;
							if (!any) {
								next.range = s.range;
								any        = true;
							} else {
								next.range = JoinU32Ranges(next.range, s.range);
							}
						}
						next.defined = any;
						break;
					}
					case ValueOpcode::SelectU32: {
						const auto cond = eval_bool(u1_args[node.u1_off]);
						if (cond == BoolState::True) {
							next = eval_u32(u32_args[node.u32_off]);
						} else if (cond == BoolState::False) {
							next = eval_u32(u32_args[node.u32_off + 1u]);
						} else if (cond == BoolState::Top) {
							const auto t = eval_u32(u32_args[node.u32_off]);
							const auto f = eval_u32(u32_args[node.u32_off + 1u]);
							if (t.defined && f.defined) {
								next = {true, JoinU32Ranges(t.range, f.range)};
							} else if (t.defined) {
								next = t;
							} else if (f.defined) {
								next = f;
							}
						}
						break;
					}
					case ValueOpcode::BitwiseAnd32: {
						const auto a = eval_u32(u32_args[node.u32_off]);
						const auto b = eval_u32(u32_args[node.u32_off + 1u]);
						if (a.defined && b.defined) next = {true, RangeForBitAnd(a.range, b.range)};
						break;
					}
					case ValueOpcode::BitwiseOr32: {
						const auto a = eval_u32(u32_args[node.u32_off]);
						const auto b = eval_u32(u32_args[node.u32_off + 1u]);
						if (a.defined && b.defined) next = {true, RangeForBitOr(a.range, b.range)};
						break;
					}
					case ValueOpcode::BitFieldUExtract: {
						const auto s = eval_u32(u32_args[node.u32_off]);
						if (s.defined) {
							next = {true, RangeForBfeU32(s.range, Arg(*node.inst, 1), Arg(*node.inst, 2))};
						}
						break;
					}
					case ValueOpcode::ShiftRightLogical32: {
						const auto s = eval_u32(u32_args[node.u32_off]);
						if (s.defined) next = {true, RangeForShrU32(s.range, Arg(*node.inst, 1))};
						break;
					}
					case ValueOpcode::ShiftLeftLogical32: {
						const auto s = eval_u32(u32_args[node.u32_off]);
						if (s.defined) next = {true, RangeForShlU32(s.range, Arg(*node.inst, 1))};
						break;
					}
					case ValueOpcode::UMin32: {
						const auto a = eval_u32(u32_args[node.u32_off]);
						const auto b = eval_u32(u32_args[node.u32_off + 1u]);
						if (a.defined && b.defined) {
							const uint32_t max_val = std::min(a.range.max, b.range.max);
							next = {true, {std::min(a.range.min, b.range.min), max_val,
							               max_val == 0u ? 0u : 1u}};
						}
						break;
					}
					case ValueOpcode::UMax32: {
						const auto a = eval_u32(u32_args[node.u32_off]);
						const auto b = eval_u32(u32_args[node.u32_off + 1u]);
						if (a.defined && b.defined) {
							const uint32_t max_val = std::max(a.range.max, b.range.max);
							next = {true, {std::max(a.range.min, b.range.min), max_val,
							               max_val == 0u ? 0u : 1u}};
						}
						break;
					}
					case ValueOpcode::ConvertU32U8: next = {true, {0u, 0xffu, 1u}}; break;
					case ValueOpcode::ConvertU32U16: next = {true, {0u, 0xffffu, 1u}}; break;
					case ValueOpcode::IAdd32: {
						const auto a = eval_u32(u32_args[node.u32_off]);
						const auto b = eval_u32(u32_args[node.u32_off + 1u]);
						if (a.defined && b.defined) next = {true, RangeForAddU32(a.range, b.range)};
						break;
					}
					case ValueOpcode::IMul32: {
						const auto a = eval_u32(u32_args[node.u32_off]);
						const auto b = eval_u32(u32_args[node.u32_off + 1u]);
						if (a.defined && b.defined) next = {true, RangeForMulU32(a.range, b.range)};
						break;
					}
					case ValueOpcode::ReadFirstLane:
					case ValueOpcode::ReadLane: next = eval_u32(u32_args[node.u32_off]); break;
					default: break;
				}
				auto& cur = u32_states[node.dense_id];
				if (next != cur) {
					next.changes = cur.changes + 1u;
					if (node.op == ValueOpcode::Phi && next.changes >= 4u) {
						next.range = FullU32Range;
					}
					if (next != cur) {
						cur     = next;
						changed = true;
					}
				}
			} else {
				BoolState next = BoolState::Bottom;
				switch (node.op) {
					case ValueOpcode::Phi:
						for (uint32_t k = 0; k < node.u1_cnt; ++k) {
							next = join_bool(next, eval_bool(u1_args[node.u1_off + k]));
						}
						break;
					case ValueOpcode::SelectU1: {
						const auto cond = eval_bool(u1_args[node.u1_off]);
						if (cond == BoolState::True) next = eval_bool(u1_args[node.u1_off + 1u]);
						else if (cond == BoolState::False) next = eval_bool(u1_args[node.u1_off + 2u]);
						else if (cond == BoolState::Top) {
							next = join_bool(eval_bool(u1_args[node.u1_off + 1u]),
							                 eval_bool(u1_args[node.u1_off + 2u]));
						}
						break;
					}
					case ValueOpcode::LogicalAnd: {
						const auto a = eval_bool(u1_args[node.u1_off]);
						const auto b = eval_bool(u1_args[node.u1_off + 1u]);
						if (a == BoolState::False || b == BoolState::False) next = BoolState::False;
						else if (a == BoolState::Bottom || b == BoolState::Bottom)
							next = BoolState::Bottom;
						else if (a == BoolState::True && b == BoolState::True)
							next = BoolState::True;
						else next = BoolState::Top;
						break;
					}
					case ValueOpcode::LogicalOr: {
						const auto a = eval_bool(u1_args[node.u1_off]);
						const auto b = eval_bool(u1_args[node.u1_off + 1u]);
						if (a == BoolState::True || b == BoolState::True) next = BoolState::True;
						else if (a == BoolState::Bottom || b == BoolState::Bottom)
							next = BoolState::Bottom;
						else if (a == BoolState::False && b == BoolState::False)
							next = BoolState::False;
						else next = BoolState::Top;
						break;
					}
					case ValueOpcode::LogicalNot: {
						const auto a = eval_bool(u1_args[node.u1_off]);
						next         = a == BoolState::True  ? BoolState::False
						             : a == BoolState::False ? BoolState::True
						                                     : a;
						break;
					}
					case ValueOpcode::ConditionRef: next = eval_bool(u1_args[node.u1_off]); break;
					default: {
						if (node.same_args) {
							const bool res = node.op == ValueOpcode::IEqual32 ||
							                 node.op == ValueOpcode::ULessThanEqual32 ||
							                 node.op == ValueOpcode::UGreaterThanEqual32;
							next = res ? BoolState::True : BoolState::False;
							break;
						}
						const auto a = eval_u32(u32_args[node.u32_off]);
						const auto b = eval_u32(u32_args[node.u32_off + 1u]);
						if (a.defined && b.defined) {
							const auto res = EvaluateRangeCompare(node.op, a.range, b.range);
							next           = res ? (*res ? BoolState::True : BoolState::False)
							                     : BoolState::Top;
						}
						break;
					}
				}
				auto& cur = bool_states[node.dense_id];
				if (next != cur) {
					cur     = next;
					changed = true;
				}
			}
		}
		if (!changed) break;
	}

	bool folded = false;
	for (uint32_t i = 0; i < static_cast<uint32_t>(u1_nodes.size()); ++i) {
		auto&      inst = *u1_nodes[i];
		const auto op   = inst.GetOpcode();
		if (op == ValueOpcode::Phi || op == ValueOpcode::ConditionRef) continue;
		const auto state = bool_states[i];
		if (state != BoolState::True && state != BoolState::False) continue;
		worklist.QueueUsers(inst.Uses());
		Replace(inst, Value(state == BoolState::True));
		folded = true;
	}
	return folded;
}

bool PromoteFloatSelectAndPhi(const BlockList& blocks, FoldWorklist& worklist) {
	std::vector<Inst*>                        cands;
	std::unordered_map<const Inst*, uint32_t> cand_id;
	for (auto* block: blocks) {
		for (auto& inst: *block) {
			const auto op = inst.GetOpcode();
			if (op == ValueOpcode::SelectU32 ||
			    (op == ValueOpcode::Phi && inst.GetType() == Type::U32)) {
				const auto id = static_cast<uint32_t>(cands.size());
				cands.push_back(&inst);
				cand_id.emplace(&inst, id);
			}
		}
	}
	if (cands.empty()) return false;

	const size_t         m = cands.size();
	std::vector<uint8_t> float_only(m, 1u);
	std::vector<uint32_t> queue;
	for (size_t id = 0; id < m; ++id) {
		const Inst* c     = cands[id];
		bool        valid = c->HasUses();
		if (valid) {
			for (const auto& u: c->Uses()) {
				const auto* user = u.user;
				const auto  uop  = user->GetOpcode();
				if (uop == ValueOpcode::Identity || uop == ValueOpcode::BitCastF32U32) continue;
				if (!user->HasUses() && !user->MayHaveSideEffects()) continue;
				if (cand_id.contains(user) && (uop == ValueOpcode::Phi || u.operand != 0u)) continue;
				valid = false;
				break;
			}
		}
		if (!valid) {
			float_only[id] = 0u;
			queue.push_back(static_cast<uint32_t>(id));
		}
	}
	for (size_t head = 0; head < queue.size(); ++head) {
		const Inst* c         = cands[queue[head]];
		const size_t first    = c->GetOpcode() == ValueOpcode::Phi ? 0u : 1u;
		for (size_t k = first; k < c->NumArgs(); ++k) {
			const auto* p = Arg(*c, k).TryInstruction();
			if (p == nullptr) continue;
			if (const auto it = cand_id.find(p); it != cand_id.end() && float_only[it->second] != 0u) {
				float_only[it->second] = 0u;
				queue.push_back(it->second);
			}
		}
	}

	std::vector<uint32_t> parent(m);
	std::iota(parent.begin(), parent.end(), 0u);
	const auto find = [&](uint32_t x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x         = parent[x];
		}
		return x;
	};
	const auto unite = [&](uint32_t a, uint32_t b) {
		a = find(a);
		b = find(b);
		if (a != b) parent[b] = a;
	};
	for (size_t id = 0; id < m; ++id) {
		if (float_only[id] == 0u) continue;
		const Inst*  c     = cands[id];
		const size_t first = c->GetOpcode() == ValueOpcode::Phi ? 0u : 1u;
		for (size_t k = first; k < c->NumArgs(); ++k) {
			const auto* p = Arg(*c, k).TryInstruction();
			if (p == nullptr) continue;
			if (const auto it = cand_id.find(p); it != cand_id.end() && float_only[it->second] != 0u) {
				unite(static_cast<uint32_t>(id), it->second);
			}
		}
	}

	std::unordered_map<uint32_t, std::vector<uint32_t>> groups;
	for (size_t id = 0; id < m; ++id) {
		if (float_only[id] != 0u) groups[find(static_cast<uint32_t>(id))].push_back(static_cast<uint32_t>(id));
	}

	std::vector<uint32_t> comp_mark(m, 0u);
	uint32_t              comp_stamp = 0u;
	std::vector<Inst*>    f32_node(m, nullptr);
	bool                  promoted = false;

	for (const auto& [root, comp]: groups) {
		(void)root;
		comp_stamp++;
		for (const auto id: comp) comp_mark[id] = comp_stamp;

		size_t                           saved = 0u;
		std::unordered_set<const Inst*>  bitcast_inputs;
		std::unordered_set<Inst*>        ext_u32_inputs;
		bool                             ok = true;
		for (const auto id: comp) {
			Inst* n = cands[id];
			if (!worklist.locations.contains(n)) {
				ok = false;
				break;
			}
			for (const auto& u: n->Uses()) {
				if (u.user->GetOpcode() == ValueOpcode::BitCastF32U32) saved++;
			}
			const size_t first = n->GetOpcode() == ValueOpcode::Phi ? 0u : 1u;
			for (size_t k = first; k < n->NumArgs(); ++k) {
				const Value v = Arg(*n, k);
				if (IsImmediate(v, Type::U32)) continue;
				Inst* p = v.TryInstruction();
				if (p == nullptr) {
					ok = false;
					break;
				}
				if (const auto pit = cand_id.find(p);
				    pit != cand_id.end() && comp_mark[pit->second] == comp_stamp) {
					continue;
				}
				if (p->GetOpcode() == ValueOpcode::BitCastU32F32) {
					bitcast_inputs.insert(p);
					continue;
				}
				if (p->Parent() == nullptr ||
				    (p->GetOpcode() != ValueOpcode::Phi && !worklist.locations.contains(p))) {
					ok = false;
					break;
				}
				ext_u32_inputs.insert(p);
			}
			if (!ok) break;
		}
		if (!ok) continue;

		for (const auto* bc: bitcast_inputs) {
			const bool all_in_comp = std::ranges::all_of(bc->Uses(), [&](const Use& u) {
				const auto uop = u.user->GetOpcode();
				if (uop == ValueOpcode::Identity) return true;
				if (!u.user->HasUses() && !u.user->MayHaveSideEffects()) return true;
				const auto it = cand_id.find(u.user);
				return it != cand_id.end() && comp_mark[it->second] == comp_stamp &&
				       (uop == ValueOpcode::Phi || u.operand != 0u);
			});
			if (all_in_comp) saved++;
		}
		if (saved <= ext_u32_inputs.size()) continue;

		std::unordered_map<Inst*, Inst*> ext_cast;
		ext_cast.reserve(ext_u32_inputs.size());
		for (auto* ext: ext_u32_inputs) {
			Block::iterator insert_pos;
			if (ext->GetOpcode() == ValueOpcode::Phi) {
				insert_pos = ext->Parent()->begin();
				for (auto it = insert_pos; it != ext->Parent()->end(); ++it) {
					if (it->GetOpcode() == ValueOpcode::Phi) insert_pos = std::next(it);
					else if (it->GetOpcode() != ValueOpcode::Identity) break;
				}
			} else {
				insert_pos = std::next(worklist.locations.at(ext).instruction);
			}
			const auto cast_it = worklist.Prepend(*ext->Parent(), insert_pos,
			                                      ValueOpcode::BitCastF32U32, {Value(ext)});
			ext_cast.emplace(ext, &*cast_it);
		}

		for (const auto id: comp) {
			Inst*      n   = cands[id];
			const auto pos = worklist.locations.at(n).instruction;
			if (n->GetOpcode() == ValueOpcode::Phi) {
				auto np = worklist.Prepend(*n->Parent(), n->Parent()->begin(), ValueOpcode::Phi);
				np->SetFlags(Type::F32);
				f32_node[id] = &*np;
			} else {
				auto ns = worklist.Prepend(*n->Parent(), pos, ValueOpcode::SelectF32,
				                           {Arg(*n, 0), Value::F32(0.0f), Value::F32(0.0f)});
				f32_node[id] = &*ns;
			}
		}

		const auto convert_val = [&](Value v) -> Value {
			v = v.Resolve();
			if (IsImmediate(v, Type::U32)) return Value::F32(std::bit_cast<float>(v.U32()));
			Inst* p = v.TryInstruction();
			if (const auto pit = cand_id.find(p);
			    pit != cand_id.end() && comp_mark[pit->second] == comp_stamp) {
				return Value(f32_node[pit->second]);
			}
			if (p->GetOpcode() == ValueOpcode::BitCastU32F32) return Arg(*p, 0);
			return Value(ext_cast.at(p));
		};

		for (const auto id: comp) {
			Inst* n = cands[id];
			if (n->GetOpcode() == ValueOpcode::Phi) {
				for (size_t k = 0; k < n->NumArgs(); ++k) {
					f32_node[id]->AddPhiOperand(n->PhiBlock(k), convert_val(Arg(*n, k)));
				}
			} else {
				f32_node[id]->SetArg(1, convert_val(Arg(*n, 1)));
				f32_node[id]->SetArg(2, convert_val(Arg(*n, 2)));
			}
		}

		for (const auto id: comp) {
			Inst*                  n = cands[id];
			const std::vector<Use> uses(n->Uses().begin(), n->Uses().end());
			for (const auto& use: uses) {
				if (use.user->GetOpcode() == ValueOpcode::BitCastF32U32) {
					worklist.QueueUsers(use.user->Uses());
					Replace(*use.user, Value(f32_node[id]));
				}
			}
		}
		promoted = true;
	}
	return promoted;
}

} // namespace

void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size, bool aggressive) {
	FoldWorklist worklist;
	LaneMaskProjection mask_projection(worklist);
	for (auto* block: blocks)
		for (auto inst = block->begin(); inst != block->end(); ++inst) worklist.Register(inst);
	std::vector<Use> users;
	std::vector<Value> operands;
	const auto drain = [&] {
		do {
			for (size_t i = 0; i < worklist.pending.size(); ++i) {
				auto& location    = *worklist.pending[i];
				location.queued = false;
				auto*      inst   = &*location.instruction;
				const auto opcode = inst->GetOpcode();
				if (opcode == ValueOpcode::Identity || opcode == ValueOpcode::Void) continue;
				users.assign(inst->Uses().begin(), inst->Uses().end());
				operands.clear();
				for (size_t arg = 0; arg < inst->NumArgs(); ++arg) operands.push_back(inst->Arg(arg));
				mask_projection.Fold(*inst, wave_size);
				FoldInstruction(*inst->Parent(), location.instruction, worklist, aggressive);
				bool changed = opcode != inst->GetOpcode() || operands.size() != inst->NumArgs();
				for (size_t arg = 0; !changed && arg < operands.size(); ++arg)
					changed = operands[arg] != inst->Arg(arg);
				if (changed) {
					worklist.QueueUsers(users);
					worklist.Queue(location);
				}
			}
			worklist.pending.clear();
			worklist.FlushUsers();
		} while (!worklist.pending.empty());
	};
	drain();
	if (aggressive) {
		for (size_t pass = 0; pass < 3u; ++pass) {
			if (FoldOptimisticU32Ranges(blocks, worklist)) {
				worklist.FlushUsers();
				drain();
			}
			if (!EliminateGuardedSelects(blocks, worklist)) break;
			worklist.FlushUsers();
			drain();
		}
		if (PromoteFloatSelectAndPhi(blocks, worklist)) {
			worklist.FlushUsers();
			drain();
		}
	}
	// Normalize retained PHI/select values only after every supported field read has
	// been lowered; direct raw consumers remain unsupported.
	bool normalized;
	do {
		normalized = false;
		for (auto* source: worklist.lowered_ancillary) {
			if (source->GetOpcode() == ValueOpcode::Identity) continue;
			const bool retained_only = std::ranges::all_of(source->Uses(), [](const Use& use) {
				const auto& user = *use.user;
				if (!user.HasUses() && !user.MayHaveSideEffects()) {
					return true;
				}
				return user.GetOpcode() == ValueOpcode::Phi ||
				       (user.GetOpcode() == ValueOpcode::SelectU32 && use.operand == 2u);
			});
			if (retained_only) {
				worklist.QueueUsers(source->Uses());
				Replace(*source, Value(0u));
				normalized = true;
			}
		}
		drain();
	} while (normalized);
	// Repeated register reads share a bitcast only within the same SSA block.
	std::unordered_map<const Inst*, Inst*> bitcasts;
	for (auto* block: blocks) {
		bitcasts.clear();
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::BitCastF32U32 &&
			    inst.GetOpcode() != ValueOpcode::BitCastU32F32) continue;
			const auto* source = Arg(inst, 0).TryInstruction();
			if (source == nullptr) continue;
			const auto [entry, inserted] = bitcasts.try_emplace(source, &inst);
			if (!inserted) Replace(inst, Value(entry->second));
		}
	}
}

namespace {

bool IsUnsafePerLaneOpcode(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::Ballot:
		case ValueOpcode::LiveMask:
		case ValueOpcode::ReadFirstLane:
		case ValueOpcode::ReadLane:
		case ValueOpcode::SubgroupReduceU32:
		case ValueOpcode::WriteLane:
		case ValueOpcode::Permlane16U32:
		case ValueOpcode::PermuteU32:
		case ValueOpcode::BpermuteU32:
		case ValueOpcode::DppMoveU32:
		case ValueOpcode::DppUpdateU32:
		case ValueOpcode::SwizzleU32:
		case ValueOpcode::WqmU64:
		case ValueOpcode::Barrier:
		case ValueOpcode::MeshAllocate:
		case ValueOpcode::SetAttribute:
		case ValueOpcode::Sendmsg:
		case ValueOpcode::TtraceData:
		case ValueOpcode::DataAppend:
		case ValueOpcode::DataConsume:
		case ValueOpcode::StoreCompletion:
		case ValueOpcode::DataCacheInvalidate:
		case ValueOpcode::ImageQueryLod: return true;
		default: return false;
	}
}

} // namespace

bool LowerDivergentExecRegions(Program& program, bool single_lane) {
	// Shaders running 1:1 on host lanes (all wave32 shaders, non-workgroup stages, or wave64 hosts)
	// can branch per lane across structured SESE EXEC regions without cross-lane/barrier/export/
	// implicit-LOD ops instead of running inactive lanes under software Select/OpSelectionMerge.
	if (!single_lane || program.dispatcher_fallback || program.blocks.size() < 3u) {
		return false;
	}

	const DominanceInfo dom(program.blocks, false);
	const auto&         rpo = dom.Rpo();
	if (rpo.size() < 3u) return false;

	std::unordered_map<const Block*, size_t> block_id;
	block_id.reserve(rpo.size());
	for (size_t i = 0; i < rpo.size(); ++i) block_id.emplace(rpo[i], i);

	bool                 changed = false;
	std::vector<uint8_t> in_region(rpo.size(), 0u);
	std::vector<Block*>  region_blocks;

	for (size_t hi = 0; hi < rpo.size(); ++hi) {
		auto* H = const_cast<Block*>(rpo[hi]);
		if (H->terminator.kind != CFG::TerminatorKind::ConditionalBranch ||
		    H->terminator.loop_header) {
			continue;
		}
		Block* M = H->terminator.merge_block;
		Block* T = H->terminator.true_block;
		Block* F = H->terminator.false_block;
		if (M == nullptr || T == nullptr || F == nullptr || T == F) continue;

		Inst* cond_ref = H->condition.Resolve().TryInstruction();
		Inst* not_inst = nullptr;
		bool  negated  = false;
		if (cond_ref != nullptr && cond_ref->GetOpcode() == ValueOpcode::LogicalNot) {
			not_inst = cond_ref;
			cond_ref = cond_ref->Arg(0).Resolve().TryInstruction();
			negated  = true;
		}
		if (cond_ref == nullptr || cond_ref->GetOpcode() != ValueOpcode::ConditionRef ||
		    cond_ref->Flags<CFG::BranchCondition>() != CFG::BranchCondition::ExecNonZero) {
			continue;
		}

		Block* region_entry = negated ? F : T;
		Block* skip_target  = negated ? T : F;
		if (skip_target != M || region_entry == H || region_entry == M) continue;
		const auto entry_preds = region_entry->ImmPredecessors();
		if (entry_preds.size() != 1u || entry_preds[0] != H) continue;
		const auto entry_it = block_id.find(region_entry);
		if (entry_it == block_id.end()) continue;

		const Value active = cond_ref->Arg(0).Resolve();
		if (active.TryInstruction() == nullptr) continue;

		std::fill(in_region.begin(), in_region.end(), 0u);
		region_blocks.clear();
		in_region[entry_it->second] = 1u;
		region_blocks.push_back(region_entry);

		bool sese_valid = true;
		for (size_t idx = 0; idx < region_blocks.size() && sese_valid; ++idx) {
			const Block* B = region_blocks[idx];
			if (B->terminator.kind != CFG::TerminatorKind::Branch &&
			    B->terminator.kind != CFG::TerminatorKind::ConditionalBranch) {
				sese_valid = false;
				break;
			}
			const auto succs = B->ImmSuccessors();
			if (succs.empty()) {
				sese_valid = false;
				break;
			}
			for (const Block* succ: succs) {
				if (succ == nullptr || succ == H) {
					sese_valid = false;
					break;
				}
				if (succ == M) continue;
				const auto sit = block_id.find(succ);
				if (sit == block_id.end()) {
					sese_valid = false;
					break;
				}
				if (in_region[sit->second] == 0u) {
					in_region[sit->second] = 1u;
					region_blocks.push_back(const_cast<Block*>(succ));
				}
			}
		}
		if (!sese_valid) continue;

		for (const Block* B: region_blocks) {
			if (B != region_entry) {
				for (const Block* pred: B->ImmPredecessors()) {
					const auto pit = block_id.find(pred);
					if (pit == block_id.end() || in_region[pit->second] == 0u) {
						sese_valid = false;
						break;
					}
				}
				if (!sese_valid) break;
			}
			if (const Block* mb = B->terminator.merge_block; mb != nullptr && mb != M) {
				const auto mit = block_id.find(mb);
				if (mit == block_id.end() || in_region[mit->second] == 0u) {
					sese_valid = false;
					break;
				}
			}
			if (const Block* cb = B->terminator.continue_block; cb != nullptr) {
				const auto cit = block_id.find(cb);
				if (cit == block_id.end() || in_region[cit->second] == 0u) {
					sese_valid = false;
					break;
				}
			}
		}
		if (!sese_valid) continue;

		const auto implies_active = [&](auto&& self, Value cond, size_t depth) -> bool {
			cond = cond.Resolve();
			if (cond == active) return true;
			if (depth >= 8u) return false;
			const auto* ci = cond.TryInstruction();
			if (ci == nullptr) return false;
			if (ci->GetOpcode() == ValueOpcode::LogicalAnd) {
				return self(self, ci->Arg(0), depth + 1u) || self(self, ci->Arg(1), depth + 1u);
			}
			return false;
		};

		// Verifies that when `active == false`, evaluating `v` yields `target` (mod cast state:
		// 0=direct, 1=expect BitCastF32U32(target), 2=expect BitCastU32F32(target)).
		const auto matches_when_inactive = [&](auto&& self, Value v, Value target,
		                                       uint8_t expect_cast, size_t depth,
		                                       std::unordered_set<const Inst*>& visited) -> bool {
			if (depth >= 16u) return false;
			v      = v.Resolve();
			target = target.Resolve();
			if (expect_cast == 0u && v == target) return true;
			if (expect_cast == 0u && IsImmediate(target, Type::U1) && !target.U1() &&
			    implies_active(implies_active, v, 0u)) {
				return true;
			}
			if (expect_cast == 1u) {
				if (IsImmediate(v, Type::F32) && IsImmediate(target, Type::U32)) {
					return std::bit_cast<uint32_t>(v.F32Value()) == target.U32();
				}
			} else if (expect_cast == 2u) {
				if (IsImmediate(v, Type::U32) && IsImmediate(target, Type::F32)) {
					return v.U32() == std::bit_cast<uint32_t>(target.F32Value());
				}
			}
			const auto* vi = v.TryInstruction();
			if (vi == nullptr) return false;
			const auto vop = vi->GetOpcode();
			if (expect_cast == 1u && vop == ValueOpcode::BitCastF32U32) {
				return self(self, vi->Arg(0), target, 0u, depth + 1u, visited);
			}
			if (expect_cast == 2u && vop == ValueOpcode::BitCastU32F32) {
				return self(self, vi->Arg(0), target, 0u, depth + 1u, visited);
			}
			if (expect_cast == 0u) {
				if (vop == ValueOpcode::BitCastF32U32) {
					const auto* ti = target.TryInstruction();
					if (ti != nullptr && ti->GetOpcode() == ValueOpcode::BitCastF32U32) {
						return self(self, vi->Arg(0), ti->Arg(0), 0u, depth + 1u, visited);
					}
					return self(self, vi->Arg(0), target, 2u, depth + 1u, visited);
				}
				if (vop == ValueOpcode::BitCastU32F32) {
					const auto* ti = target.TryInstruction();
					if (ti != nullptr && ti->GetOpcode() == ValueOpcode::BitCastU32F32) {
						return self(self, vi->Arg(0), ti->Arg(0), 0u, depth + 1u, visited);
					}
					return self(self, vi->Arg(0), target, 1u, depth + 1u, visited);
				}
			}
			if (IsSelectOpcode(vop) && implies_active(implies_active, vi->Arg(0), 0u)) {
				return self(self, vi->Arg(2), target, expect_cast, depth + 1u, visited);
			}
			if (vop == ValueOpcode::Phi) {
				const auto pit = block_id.find(vi->Parent());
				if (pit == block_id.end() || in_region[pit->second] == 0u) return false;
				if (!visited.insert(vi).second) return true;
				for (size_t k = 0; k < vi->NumArgs(); ++k) {
					if (!self(self, vi->Arg(k), target, expect_cast, depth + 1u, visited)) {
						return false;
					}
				}
				return true;
			}
			return false;
		};

		const auto in_region_block = [&](const Block* b) {
			const auto it = block_id.find(b);
			return it != block_id.end() && in_region[it->second] != 0u;
		};

		const auto has_bad_external_use = [&](auto&& self, const Inst& inst, size_t depth) -> bool {
			if (depth >= 16u) return true;
			for (const auto& use: inst.Uses()) {
				const Inst* user = use.user;
				if (user->GetOpcode() == ValueOpcode::Identity) {
					if (self(self, *user, depth + 1u)) return true;
					continue;
				}
				if (!user->HasUses() && !user->MayHaveSideEffects()) continue;
				if (in_region_block(user->Parent())) continue;
				if (user->Parent() == M && user->GetOpcode() == ValueOpcode::Phi) continue;
				return true;
			}
			return false;
		};

		bool region_safe = true;
		for (const Block* B: region_blocks) {
			for (const auto& inst: *B) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::Identity || op == ValueOpcode::Void) continue;
				if (IsUnsafePerLaneOpcode(op)) {
					region_safe = false;
					break;
				}
				if (op == ValueOpcode::ConditionRef) {
					const auto kind = inst.Flags<CFG::BranchCondition>();
					if (kind != CFG::BranchCondition::ScalarInstruction) {
						std::unordered_set<const Inst*> visited;
						if (kind != CFG::BranchCondition::ExecNonZero ||
						    !matches_when_inactive(matches_when_inactive, inst.Arg(0),
						                           Value(false), 0u, 0u, visited)) {
							region_safe = false;
							break;
						}
					}
				} else if (op == ValueOpcode::ImageSampleRaw &&
				           program.stage == ShaderType::Pixel) {
					const auto& mem = program.memory_info.at(inst.Flags<MemoryFlags>().index);
					constexpr uint32_t ExplicitSampleFlags = Decoder::ImageSampleFlagDerivative |
					                                       Decoder::ImageSampleFlagLod |
					                                       Decoder::ImageSampleFlagLevelZero;
					if ((mem.image_sample_flags & ExplicitSampleFlags) == 0u) {
						region_safe = false;
						break;
					}
				}
				if (has_bad_external_use(has_bad_external_use, inst, 0u)) {
					region_safe = false;
					break;
				}
			}
			if (!region_safe) break;
		}
		if (!region_safe) continue;

		for (const auto& inst: *M) {
			if (inst.GetOpcode() == ValueOpcode::Identity) continue;
			if (inst.GetOpcode() != ValueOpcode::Phi) break;
			if (!inst.HasUses()) continue;
			Value found_from_h;
			bool  has_from_h = false;
			for (size_t k = 0; k < inst.NumArgs(); ++k) {
				if (inst.PhiBlock(k) == H) {
					found_from_h = inst.Arg(k);
					has_from_h   = true;
					break;
				}
			}
			if (!has_from_h) {
				region_safe = false;
				break;
			}
			for (size_t k = 0; k < inst.NumArgs(); ++k) {
				if (!in_region_block(inst.PhiBlock(k))) continue;
				std::unordered_set<const Inst*> visited;
				if (!matches_when_inactive(matches_when_inactive, inst.Arg(k), found_from_h, 0u, 0u,
				                           visited)) {
					region_safe = false;
					break;
				}
			}
			if (!region_safe) break;
		}
		if (!region_safe) continue;

		// Lower H's branch vote to a direct per-lane branch, reusing existing ConditionRef when private.
		bool can_mutate_cond_ref = cond_ref->Parent() == H;
		for (const Block* B2: rpo) {
			if (B2 == H) continue;
			const auto* c2 = B2->condition.Resolve().TryInstruction();
			if (c2 == cond_ref || (not_inst != nullptr && c2 == not_inst)) {
				can_mutate_cond_ref = false;
				break;
			}
		}
		const auto check_private_uses = [&](auto&& self, const Inst& inst, bool allow_not,
		                                    size_t depth) -> bool {
			if (depth >= 16u) return false;
			for (const auto& use: inst.Uses()) {
				const Inst* user = use.user;
				if (user->GetOpcode() == ValueOpcode::Identity) {
					if (!self(self, *user, allow_not, depth + 1u)) return false;
					continue;
				}
				if (allow_not && user == not_inst) continue;
				if (user->Parent() == H && user->GetOpcode() == ValueOpcode::Reference) continue;
				return false;
			}
			return true;
		};
		if (can_mutate_cond_ref) {
			can_mutate_cond_ref = check_private_uses(check_private_uses, *cond_ref, not_inst != nullptr, 0u) &&
			                      (not_inst == nullptr ||
			                       (not_inst->Parent() == H &&
			                        check_private_uses(check_private_uses, *not_inst, false, 0u)));
		}

		if (can_mutate_cond_ref) {
			cond_ref->SetFlags(CFG::BranchCondition::ScalarInstruction);
		} else {
			Block::iterator insert_pos = H->end();
			Inst*           ref_inst   = nullptr;
			for (auto it = H->begin(); it != H->end(); ++it) {
				if (it->GetOpcode() == ValueOpcode::Reference &&
				    it->Arg(0).Resolve().TryInstruction() == (not_inst != nullptr ? not_inst : cond_ref)) {
					insert_pos = it;
					ref_inst   = &*it;
					break;
				}
			}
			auto new_ref_it = H->PrependNewInst(
			    insert_pos, ValueOpcode::ConditionRef, {active},
			    static_cast<uint64_t>(CFG::BranchCondition::ScalarInstruction));
			Value new_cond(&*new_ref_it);
			if (negated) {
				auto new_not_it = H->PrependNewInst(insert_pos, ValueOpcode::LogicalNot, {new_cond});
				new_cond        = Value(&*new_not_it);
			}
			H->condition = new_cond;
			if (ref_inst != nullptr) {
				ref_inst->SetArg(0, new_cond);
			} else {
				H->AppendNewInst(ValueOpcode::Reference, {new_cond});
			}
		}

		for (Block* B: region_blocks) {
			for (auto& inst: *B) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::Identity || op == ValueOpcode::Void) continue;
				for (size_t k = 0; k < inst.NumArgs(); ++k) {
					if (inst.Arg(k).Resolve() == active) inst.SetArg(k, Value(true));
				}
				if (op == ValueOpcode::LogicalAnd) {
					if (Arg(inst, 0) == Value(true)) {
						Replace(inst, Arg(inst, 1));
					} else if (Arg(inst, 1) == Value(true)) {
						Replace(inst, Arg(inst, 0));
					}
				} else if (IsSelectOpcode(op) && Arg(inst, 0) == Value(true)) {
					Replace(inst, Arg(inst, 1));
				} else if (op == ValueOpcode::ConditionRef && Arg(inst, 0) == Value(true)) {
					Replace(inst, Value(true));
				}
			}
			if (B->condition.Resolve() == active) B->condition = Value(true);
		}

		for (auto& inst: *M) {
			if (inst.GetOpcode() == ValueOpcode::Identity) continue;
			if (inst.GetOpcode() != ValueOpcode::Phi) break;
			for (size_t k = 0; k < inst.NumArgs(); ++k) {
				if (in_region_block(inst.PhiBlock(k)) && inst.Arg(k).Resolve() == active) {
					inst.SetArg(k, Value(true));
				}
			}
		}
		changed = true;
	}
	return changed;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
