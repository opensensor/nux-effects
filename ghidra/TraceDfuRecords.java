// Locate and decompile the device-side HID/DFU record parser.
//
// Run against the 64 KiB RTX_DFU image imported at address 0:
//   analyzeHeadless <project-dir> <project-name> \
//     -process dfu_img.bin -scriptPath ./ghidra \
//     -postScript TraceDfuRecords.java

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

public class TraceDfuRecords extends GhidraScript {
    private AddressSpace space;
    private FunctionManager functions;
    private DecompInterface decompiler;

    private Address addr(long value) {
        return space.getAddress(value & 0xffffffffL);
    }

    private Set<Long> immediates(Function function) {
        Set<Long> values = new TreeSet<>();
        InstructionIterator instructions =
            currentProgram.getListing().getInstructions(function.getBody(), true);
        while (instructions.hasNext()) {
            Instruction instruction = instructions.next();
            for (int operand = 0; operand < instruction.getNumOperands(); operand++) {
                for (Object object : instruction.getOpObjects(operand)) {
                    if (object instanceof Scalar) {
                        values.add(((Scalar) object).getUnsignedValue());
                    }
                }
            }
        }
        return values;
    }

    private List<Function> directCallers(Function target) {
        List<Function> callers = new ArrayList<>();
        Set<Long> seen = new HashSet<>();
        ReferenceIterator references =
            currentProgram.getReferenceManager().getReferencesTo(target.getEntryPoint());
        while (references.hasNext()) {
            Reference reference = references.next();
            Function caller = functions.getFunctionContaining(reference.getFromAddress());
            if (caller != null && seen.add(caller.getEntryPoint().getOffset())) {
                callers.add(caller);
            }
        }
        Collections.sort(callers,
            (left, right) -> left.getEntryPoint().compareTo(right.getEntryPoint()));
        return callers;
    }

    private List<Function> directCallees(Function source) {
        List<Function> callees = new ArrayList<>();
        Set<Long> seen = new HashSet<>();
        InstructionIterator instructions =
            currentProgram.getListing().getInstructions(source.getBody(), true);
        while (instructions.hasNext()) {
            Instruction instruction = instructions.next();
            for (Reference reference : instruction.getReferencesFrom()) {
                if (!reference.getReferenceType().isCall()) {
                    continue;
                }
                Function callee = functions.getFunctionAt(reference.getToAddress());
                if (callee != null && seen.add(callee.getEntryPoint().getOffset())) {
                    callees.add(callee);
                }
            }
        }
        Collections.sort(callees,
            (left, right) -> left.getEntryPoint().compareTo(right.getEntryPoint()));
        return callees;
    }

    private void printFunction(Function function, String reason) {
        if (function == null) {
            return;
        }
        println(String.format(
            "\n===== %s: %s @ %s size=%d =====",
            reason,
            function.getName(),
            function.getEntryPoint(),
            function.getBody().getNumAddresses()));
        println("immediates: " + immediates(function));

        StringBuilder callerLine = new StringBuilder("callers:");
        for (Function caller : directCallers(function)) {
            callerLine.append(String.format(
                " %s@%s", caller.getName(), caller.getEntryPoint()));
        }
        println(callerLine.toString());

        StringBuilder calleeLine = new StringBuilder("callees:");
        for (Function callee : directCallees(function)) {
            calleeLine.append(String.format(
                " %s@%s", callee.getName(), callee.getEntryPoint()));
        }
        println(calleeLine.toString());

        DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
        if (result != null && result.decompileCompleted()) {
            println(result.getDecompiledFunction().getC());
        } else {
            println("// decompilation failed");
        }
    }

    public void run() throws Exception {
        space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        functions = currentProgram.getFunctionManager();
        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        long[] knownCandidates = {
            0x3f88, 0x40f8, 0x42a4, 0x45d8, 0x59a8, 0x6694
        };
        for (long candidate : knownCandidates) {
            printFunction(
                functions.getFunctionContaining(addr(candidate)),
                "known candidate");
        }

        println("\n===== functions containing record/HID constants =====");
        FunctionIterator iterator = functions.getFunctions(true);
        while (iterator.hasNext()) {
            Function function = iterator.next();
            Set<Long> values = immediates(function);
            boolean reportAssembler =
                values.contains(8L) && (values.contains(60L) || values.contains(64L));
            boolean updaterCommand =
                values.contains(0x56L) || values.contains(0x41L);
            boolean flashPage =
                values.contains(512L) && (values.contains(1L) || values.contains(4L));
            boolean recordSize =
                values.contains(540L) || values.contains(0x21cL);
            if (reportAssembler || updaterCommand || flashPage || recordSize) {
                println(String.format(
                    "%s @ %s size=%d immediates=%s",
                    function.getName(),
                    function.getEntryPoint(),
                    function.getBody().getNumAddresses(),
                    values));
            }
        }
    }
}
