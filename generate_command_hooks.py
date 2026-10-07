#!/usr/bin/env python3
"""Generate the Vulkan command-buffer forwarding hooks from vk.xml."""

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EXCLUDED = {
    "vkCmdPipelineBarrier", "vkCmdWaitEvents",
    "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR",
    "vkCmdWaitEvents2", "vkCmdWaitEvents2KHR",
    "vkCmdBindTileMemoryQCOM",
}
NO_ACTION = {
    "vkCmdDispatch", "vkCmdDispatchBase", "vkCmdDispatchBaseKHR",
    "vkCmdPushConstants", "vkCmdPushConstants2", "vkCmdPushConstants2KHR",
    "vkCmdResetQueryPool", "vkCmdWriteTimestamp", "vkCmdWriteTimestamp2",
    "vkCmdWriteTimestamp2KHR",
}
MIN_EXTENSION_VERSION = {
    "vkCmdSetDispatchParametersARM": ("VK_ARM_SCHEDULING_CONTROLS_SPEC_VERSION", 2),
}


def command_name(node):
    if node.get("name"):
        return node.get("name")
    proto = node.find("proto/name")
    return proto.text if proto is not None else None


def resolve(name, commands):
    seen = set()
    while name in commands and commands[name].get("alias"):
        if name in seen:
            raise ValueError(f"alias cycle at {name}")
        seen.add(name)
        name = commands[name].get("alias")
    return name


def providers(root):
    result = {}
    core_macros = {
        feature.get("name"): "VK_VERSION_" + feature.get("number", "").replace(".", "_")
        for feature in root.findall("./feature")
        if feature.get("number") and not feature.get("name", "").startswith("VKSC_")
    }
    platform_protect = {
        platform.get("name"): platform.get("protect")
        for platform in root.findall("./platforms/platform")
    }
    for feature in root.findall("./feature"):
        if "vulkan" not in (feature.get("api") or "").split(","):
            continue
        number = feature.get("number")
        if not number or feature.get("name", "").startswith("VKSC_"):
            continue
        macro = "VK_VERSION_" + number.replace(".", "_")
        feature_guard = dependency(feature.get("depends"), core_macros)
        for req in feature.findall("./require"):
            if req.get("api") and "vulkan" not in req.get("api").split(","):
                continue
            req_guard = dependency(req.get("depends"), core_macros)
            for ref in req.findall("command"):
                result.setdefault(ref.get("name"), set()).add(
                    ((f"defined({macro})", feature_guard, req_guard), None, False, None))
    for ext in root.findall("./extensions/extension"):
        if "vulkan" not in (ext.get("supported") or "").split(","):
            continue
        macro = ext.get("name", "")
        protect = ext.get("protect") or platform_protect.get(ext.get("platform"))
        beta = ext.get("provisional") == "true"
        ext_guard = dependency(ext.get("depends"), core_macros)
        for req in ext.findall("./require"):
            if req.get("api") and "vulkan" not in req.get("api").split(","):
                continue
            req_guard = dependency(req.get("depends"), core_macros)
            for ref in req.findall("command"):
                result.setdefault(ref.get("name"), set()).add(
                    ((f"defined({macro})", ext_guard, req_guard), protect, beta,
                     MIN_EXTENSION_VERSION.get(ref.get("name")) if ext.get("name") == "VK_ARM_scheduling_controls" else None))
    return result


def dependency(expr, core_macros):
    if not expr:
        return None
    # Registry feature members constrain runtime support, not declarations.
    expr = re.sub(r"\bVk[A-Za-z0-9_]+::[A-Za-z0-9_]+", "1", expr)
    token_re = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|1|[+,()]")
    tokens = token_re.findall(expr)
    if "".join(tokens) != re.sub(r"\s+", "", expr):
        raise ValueError(f"unsupported dependency expression: {expr}")
    converted = []
    for token in tokens:
        if token == "+":
            converted.append("&&")
        elif token == ",":
            converted.append("||")
        elif token in "()":
            converted.append(token)
        elif token == "1":
            converted.append("1")
        else:
            converted.append(f"defined({core_macros.get(token, token)})")
    return "(" + " ".join(converted) + ")"


def declaration(param):
    name = param.findtext("name")
    if not name:
        raise ValueError("parameter without a name")
    raw = "".join(param.itertext()).strip()
    at = raw.rfind(name)
    if at < 0:
        raise ValueError(f"cannot format parameter {name}")
    return raw[:at].rstrip() + " " + raw[at:], name


def action(name, params):
    if name == "vkCmdCopyBuffer":
        return ["buffer(commandBuffer, srcBuffer);", "buffer(commandBuffer, dstBuffer);"]
    if name in ("vkCmdCopyBuffer2", "vkCmdCopyBuffer2KHR"):
        return ["copy-buffer-2"]
    if name == "vkCmdFillBuffer":
        return ["buffer(commandBuffer, dstBuffer);"]
    if name == "vkCmdUpdateBuffer":
        return ["buffer(commandBuffer, dstBuffer);"]
    if name == "vkCmdBindDescriptorSets":
        return ["descriptors(commandBuffer, descriptorSetCount, pDescriptorSets);"]
    if name == "vkCmdBindPipeline":
        return ["pipeline(commandBuffer, pipeline);"]
    if name == "vkCmdDispatchIndirect":
        return ["buffer(commandBuffer, buffer);"]
    if name == "vkCmdExecuteCommands":
        return ["secondary(commandBuffer, commandBufferCount, pCommandBuffers);"]
    if name in NO_ACTION:
        return []
    return ["unknown(commandBuffer);"]


def provider_expression(providers):
    alternatives = set()
    for guards, protect, beta, revision in providers:
        terms = [guard for guard in guards if guard]
        if protect:
            terms.append(f"defined({protect})")
        if beta:
            terms.append("defined(VK_ENABLE_BETA_EXTENSIONS)")
        if revision:
            macro, minimum = revision
            terms.append(f"defined({macro}) && {macro} >= {minimum}")
        alternatives.add("(" + " && ".join(terms) + ")")
    return " || ".join(sorted(alternatives))


def generate(registry):
    root = ET.parse(registry).getroot()
    nodes = [node for node in root.findall("./commands/command") if command_name(node)]
    commands = {command_name(node): node for node in nodes}
    provided = providers(root)
    selected = []
    for name, node in commands.items():
        if not name.startswith("vkCmd") or name in EXCLUDED:
            continue
        target = commands[resolve(name, commands)]
        params = target.findall("param")
        if not params or params[0].findtext("type") != "VkCommandBuffer":
            continue
        prov = sorted(provided.get(name, ()), key=repr)
        if not prov:
            continue
        ret = target.findtext("proto/type")
        declarations = [declaration(p) for p in params]
        selected.append((name, ret, declarations, prov, action(name, declarations)))

    out = ["// Generated by generate_command_hooks.py; do not edit.",
           "// Requires <tuple> from the including translation unit.",
           "template <typename> struct ZVramCommandSignature;",
           "template <typename R, typename... Args>",
           "struct ZVramCommandSignature<R (VKAPI_PTR *)(Args...)> { using ArgsTuple = std::tuple<Args...>; };"]
    for name, ret, params, prov, calls in selected:
        out += ["#if " + provider_expression(prov),
                f"using TrackedArgs_{name} = typename ZVramCommandSignature<PFN_{name}>::ArgsTuple;",
                f"VKAPI_ATTR {ret} VKAPI_CALL tracked{name}("]
        out.append("    " + ",\n    ".join(
            f"std::tuple_element_t<{i}, TrackedArgs_{name}> {param_name}"
            for i, (_, param_name) in enumerate(params)) + ") {")
        out += ["    auto d = findDevice(reinterpret_cast<VkDevice>(commandBuffer));",
                f"    auto next = d && d->gdpa ? reinterpret_cast<PFN_{name}>(d->gdpa(d->handle, \"{name}\")) : nullptr;"]
        if ret == "VkResult":
            out.append("    if (!next) return VK_ERROR_INITIALIZATION_FAILED;")
        else:
            out.append("    if (!next) return;")
        out += ["    if (d && d->selectiveRestore) {",
                "        try {",
                "            std::lock_guard<std::mutex> lock(d->mutex);",
                "            if (d->selectiveRestore) {"]
        if calls == ["copy-buffer-2"]:
            out += ["                if (!pCopyBufferInfo || pCopyBufferInfo->pNext ||",
                    "                    (pCopyBufferInfo->regionCount && !pCopyBufferInfo->pRegions)) {",
                    "                    d->submission.unknown(commandBuffer);",
                    "                } else {",
                    "                    bool unknownRegion = false;",
                    "                    for (std::uint32_t i = 0; i < pCopyBufferInfo->regionCount; ++i)",
                    "                        unknownRegion |= pCopyBufferInfo->pRegions[i].pNext != nullptr;",
                    "                    if (unknownRegion) d->submission.unknown(commandBuffer);",
                    "                    else {",
                    "                        d->submission.buffer(commandBuffer, pCopyBufferInfo->srcBuffer);",
                    "                        d->submission.buffer(commandBuffer, pCopyBufferInfo->dstBuffer);",
                    "                    }",
                    "                }"]
        else:
            out.extend("                d->submission." + call for call in calls)
        out += ["            }",
                "        } catch (const std::bad_alloc&) {",
                "            d->selectiveRestore = false;",
                "        }",
                "    }",
                ("    return next(" + ", ".join(n for _, n in params) + ");"
                 if ret != "void" else "    next(" + ", ".join(n for _, n in params) + ");"),
                "}", "#endif"]
    out += ["", "PFN_vkVoidFunction trackedCommandLookup(const char* name) {"]
    out.append("    if (!name) return nullptr;")
    for name, _, _, prov, _ in selected:
        out += ["#if " + provider_expression(prov),
                f"    if (std::strcmp(name, \"{name}\") == 0) return reinterpret_cast<PFN_vkVoidFunction>(tracked{name});",
                "#endif"]
    out += ["    return nullptr;", "}",
            "bool isTrackedCommand(const char* name) { return trackedCommandLookup(name) != nullptr; }", ""]
    return "\n".join(out), len(selected)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("registry", help="path to Vulkan vk.xml")
    parser.add_argument("output", nargs="?", default="command_hooks.inc")
    parser.add_argument("--check", action="store_true", help="fail if output is stale")
    parser.add_argument("--selfcheck", action="store_true", help="check required action coverage")
    args = parser.parse_args()
    content, count = generate(args.registry)
    if args.selfcheck:
        required = ("trackedvkCmdCopyBuffer", "trackedvkCmdCopyBuffer2",
                    "trackedvkCmdBindDescriptorSets", "trackedvkCmdDispatch",
                    "trackedvkCmdExecuteCommands")
        missing = [name for name in required if name not in content]
        if "VK_KHR_synchronization2" not in content:
            missing.append("VK_KHR_synchronization2 guard")
        if "VK_ARM_SCHEDULING_CONTROLS_SPEC_VERSION >= 2" not in content:
            missing.append("ARM scheduling-controls revision gate")
        if "if (!pCopyBufferInfo || pCopyBufferInfo->pNext ||" not in content:
            missing.append("CopyBuffer2 pNext/null fallback")
        if "pCopyBufferInfo->regionCount && !pCopyBufferInfo->pRegions" not in content:
            missing.append("CopyBuffer2 regions null fallback")
        if "unknownRegion |= pCopyBufferInfo->pRegions[i].pNext != nullptr;" not in content:
            missing.append("CopyBuffer2 region pNext fallback")
        if "VK_VK_" in content:
            missing.append("correct extension macro prefix")
        if missing:
            print("missing required hooks: " + ", ".join(missing), file=sys.stderr)
            return 1
    output = Path(args.output)
    if args.check:
        if not output.exists() or output.read_text() != content:
            print(f"{output} is stale", file=sys.stderr)
            return 1
    else:
        output.write_text(content)
    print(f"{count} command-buffer hooks {'checked' if args.check else 'generated'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
