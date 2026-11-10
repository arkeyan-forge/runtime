#include "gpu/fsr/spirv_reflect.h"
#include <algorithm>
#include <unordered_map>

namespace
{
    // The instructions and enumerants read here (SPIR-V 1.x specification).
    enum : uint32_t
    {
        OpName = 5,
        OpUndef = 1,
        OpCapability = 17,
        OpExecutionMode = 16,
        OpTypeImage = 25,
        OpTypeSampler = 26,
        OpTypeSampledImage = 27,
        OpTypeArray = 28,
        OpTypeRuntimeArray = 29,
        OpTypeStruct = 30,
        OpTypePointer = 32,
        OpTypeFunction = 33,
        OpFunctionParameter = 55,
        OpVariable = 59,
        OpImageTexelPointer = 60,
        OpLoad = 61,
        OpDecorate = 71,
        OpMemberDecorate = 72,
        OpCopyObject = 83,
        OpSelect = 169,
        OpPhi = 245,

        ExecutionModeLocalSize = 17,
        DecorationBlock = 2,
        DecorationBufferBlock = 3,
        DecorationNonWritable = 24,
        DecorationBinding = 33,
        DecorationDescriptorSet = 34,

        StorageUniformConstant = 0,
        StorageUniform = 2,
        StorageStorageBuffer = 12,

        DimBuffer = 5,

        ImageFormatUnknown = 0,
        CapabilityStorageImageReadWithoutFormat = 55,
        CapabilityStorageImageWriteWithoutFormat = 56,
    };

    struct Type
    {
        uint32_t op = 0;
        uint32_t element = 0; // arrays: the element type
        uint32_t dim = 0, sampled = 0; // images
    };

    struct Decorations
    {
        uint32_t set = 0, binding = 0;
        bool hasBinding = false, block = false, bufferBlock = false;
        uint32_t members = 0, nonWritableMembers = 0;
    };

    std::string ReadString(const uint32_t* words, size_t count)
    {
        std::string text;
        for (size_t i = 0; i < count; i++)
            for (uint32_t byte = 0; byte < 4; byte++)
            {
                const char c = char((words[i] >> (byte * 8)) & 0xFF);
                if (c == '\0')
                    return text;
                text += c;
            }
        return text;
    }
}

bool gpu::spirv::Reflect(const uint32_t* words, size_t count, Module& module)
{
    if (count < 5 || words[0] != 0x07230203)
        return false;
    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, Type> types;
    std::unordered_map<uint32_t, Decorations> decorations;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointers; // id -> storage class, pointee
    std::vector<std::pair<uint32_t, uint32_t>> variables;                // id, pointer type

    for (size_t at = 5; at < count;)
    {
        const uint32_t length = words[at] >> 16, op = words[at] & 0xFFFF;
        if (length == 0 || at + length > count)
            return false;
        const uint32_t* operands = words + at + 1;
        const uint32_t operandCount = length - 1;
        switch (op)
        {
        case OpName:
            if (operandCount >= 2)
                names[operands[0]] = ReadString(operands + 1, operandCount - 1);
            break;
        case OpExecutionMode:
            if (operandCount >= 5 && operands[1] == ExecutionModeLocalSize)
                for (uint32_t i = 0; i < 3; i++)
                    module.localSize[i] = operands[2 + i];
            break;
        case OpTypeImage:
            if (operandCount >= 7)
                types[operands[0]] = { op, 0, operands[2], operands[6] };
            break;
        case OpTypeSampler:
        case OpTypeStruct:
            if (operandCount >= 1)
                types[operands[0]] = { op };
            break;
        case OpTypeArray:
        case OpTypeRuntimeArray:
            if (operandCount >= 2)
                types[operands[0]] = { op, operands[1] };
            break;
        case OpTypePointer:
            if (operandCount >= 3)
                pointers[operands[0]] = { operands[1], operands[2] };
            break;
        case OpVariable:
            if (operandCount >= 3 && (operands[2] == StorageUniformConstant || operands[2] == StorageUniform ||
                                         operands[2] == StorageStorageBuffer))
                variables.emplace_back(operands[1], operands[0]);
            break;
        case OpDecorate:
            if (operandCount >= 2)
            {
                Decorations& d = decorations[operands[0]];
                if (operands[1] == DecorationBinding && operandCount >= 3)
                    d.binding = operands[2], d.hasBinding = true;
                else if (operands[1] == DecorationDescriptorSet && operandCount >= 3)
                    d.set = operands[2];
                else if (operands[1] == DecorationBlock)
                    d.block = true;
                else if (operands[1] == DecorationBufferBlock)
                    d.bufferBlock = true;
            }
            break;
        case OpMemberDecorate:
            if (operandCount >= 3 && operands[2] == DecorationNonWritable)
                decorations[operands[0]].nonWritableMembers++;
            break;
        default: break;
        }
        at += length;
    }

    for (const auto& [id, pointerType] : variables)
    {
        const auto decorated = decorations.find(id);
        const auto pointer = pointers.find(pointerType);
        if (decorated == decorations.end() || !decorated->second.hasBinding || pointer == pointers.end())
            continue;
        uint32_t typeId = pointer->second.second;
        auto type = types.find(typeId);
        while (type != types.end() && (type->second.op == OpTypeArray || type->second.op == OpTypeRuntimeArray))
            type = types.find(typeId = type->second.element);
        if (type == types.end())
            continue;
        Kind kind;
        if (type->second.op == OpTypeSampler)
            kind = Kind::Sampler;
        else if (type->second.op == OpTypeImage)
        {
            const bool storage = type->second.sampled == 2;
            if (type->second.dim == DimBuffer)
                kind = storage ? Kind::StorageBuffer : Kind::Buffer;
            else
                kind = storage ? Kind::StorageTexture : Kind::Texture;
        }
        else if (type->second.op == OpTypeStruct)
        {
            const Decorations& structure = decorations[typeId];
            const bool storageClassBuffer = pointer->second.first == StorageStorageBuffer;
            if (structure.bufferBlock || storageClassBuffer)
                kind = structure.nonWritableMembers > 0 ? Kind::Buffer : Kind::StorageBuffer;
            else
                kind = Kind::ConstantBuffer;
        }
        else
            continue;
        const auto name = names.find(id);
        module.bindings.push_back({ name != names.end() ? name->second : std::string(), kind, decorated->second.set,
            decorated->second.binding });
    }
    return true;
}

std::vector<uint32_t> gpu::spirv::WithoutStorageFormats(const uint32_t* words, size_t count)
{
    if (count < 5 || words[0] != 0x07230203)
        return {};
    std::vector<uint32_t> in(words, words + count);
    std::unordered_map<uint32_t, uint32_t> pointees;  // pointer type -> pointee
    std::unordered_map<uint32_t, uint32_t> variables; // variable -> pointer type
    std::vector<uint32_t> atomicVariables;            // the images an OpImageTexelPointer reads
    std::vector<size_t> storageImages;                // where each storage OpTypeImage starts
    bool hasRead = false, hasWrite = false;
    for (size_t at = 5; at < count;)
    {
        const uint32_t length = words[at] >> 16, op = words[at] & 0xFFFF;
        if (length == 0 || at + length > count)
            return {};
        const uint32_t* operands = words + at + 1;
        if (op == OpCapability && length >= 2)
        {
            hasRead |= operands[0] == CapabilityStorageImageReadWithoutFormat;
            hasWrite |= operands[0] == CapabilityStorageImageWriteWithoutFormat;
        }
        else if (op == OpTypeImage && length >= 9 && operands[6] == 2)
            storageImages.push_back(at);
        else if (op == OpTypePointer && length >= 4)
            pointees[operands[0]] = operands[2];
        else if (op == OpVariable && length >= 4)
            variables[operands[1]] = operands[0];
        else if (op == OpImageTexelPointer && length >= 4)
            atomicVariables.push_back(operands[2]);
        at += length;
    }
    std::vector<uint32_t> atomicTypes;
    for (uint32_t variable : atomicVariables)
        if (const auto v = variables.find(variable); v != variables.end())
            if (const auto p = pointees.find(v->second); p != pointees.end())
                atomicTypes.push_back(p->second);

    // Unknown formats; two image types that become the same (rg32f and r32f over float) are one type then (SPIR-V
    // forbids declaring a type twice): the later one's uses take the first one's id.
    bool changed = false;
    std::unordered_map<uint32_t, uint32_t> merged; // the later type's id -> the first one's
    std::vector<size_t> kept;
    for (size_t at : storageImages)
    {
        uint32_t& format = in[at + 8];
        if (format != ImageFormatUnknown &&
            std::find(atomicTypes.begin(), atomicTypes.end(), in[at + 1]) == atomicTypes.end())
        {
            format = ImageFormatUnknown;
            changed = true;
        }
        const size_t length = in[at] >> 16;
        const auto same = std::find_if(kept.begin(), kept.end(), [&](size_t other) {
            return in[other] == in[at] &&
                   std::equal(in.begin() + at + 2, in.begin() + at + length, in.begin() + other + 2);
        });
        if (same == kept.end())
            kept.push_back(at);
        else
            merged[in[at + 1]] = in[*same + 1];
    }
    if (!changed)
        return in;

    // The capabilities first, after the header; then the module without the merged types' declarations and names, and
    // their uses renamed: the operands that can name an image type in a compute shader (a pointer's or an array's
    // element type, a function's types, the result type of a load, a copy and the like).
    std::vector<uint32_t> out(in.begin(), in.begin() + 5);
    if (!hasRead)
        out.insert(out.end(), { 2u << 16 | OpCapability, CapabilityStorageImageReadWithoutFormat });
    if (!hasWrite)
        out.insert(out.end(), { 2u << 16 | OpCapability, CapabilityStorageImageWriteWithoutFormat });
    auto rename = [&](uint32_t& id) {
        if (const auto m = merged.find(id); m != merged.end())
            id = m->second;
    };
    for (size_t at = 5; at < count;)
    {
        const uint32_t length = in[at] >> 16, op = in[at] & 0xFFFF;
        const size_t start = out.size();
        out.insert(out.end(), in.begin() + at, in.begin() + at + length);
        uint32_t* operands = out.data() + start + 1;
        const uint32_t operandCount = length - 1;
        const bool declaresMerged = op == OpTypeImage && merged.contains(operands[0]);
        const bool namesMerged = (op == OpName || op == OpDecorate || op == OpMemberDecorate) && operandCount >= 1 &&
                                 merged.contains(operands[0]);
        if (declaresMerged || namesMerged)
            out.resize(start);
        else if (op == OpTypePointer && operandCount >= 3)
            rename(operands[2]);
        else if ((op == OpTypeArray || op == OpTypeRuntimeArray || op == OpTypeSampledImage) && operandCount >= 2)
            rename(operands[1]);
        else if (op == OpTypeFunction || op == OpTypeStruct)
            for (uint32_t i = 1; i < operandCount; i++)
                rename(operands[i]);
        else if ((op == OpUndef || op == OpFunctionParameter || op == OpLoad || op == OpCopyObject || op == OpSelect ||
                     op == OpPhi) &&
                 operandCount >= 1)
            rename(operands[0]);
        at += length;
    }
    return out;
}
