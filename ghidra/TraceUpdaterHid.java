// Trace application-level callers of hid_write/hid_read in the official NUX updater.
// Run with analyzeHeadless using this directory as -scriptPath.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

import java.util.LinkedHashSet;
import java.util.Set;

public class TraceUpdaterHid extends GhidraScript {
    private DecompInterface decompiler;
    private FunctionManager functions;
    private ReferenceManager references;

    private Set<Function> callersOf(Function target) {
        Set<Function> result = new LinkedHashSet<>();
        ReferenceIterator iterator = references.getReferencesTo(target.getEntryPoint());
        while (iterator.hasNext()) {
            Reference reference = iterator.next();
            if (!reference.getReferenceType().isCall() &&
                !reference.getReferenceType().isJump()) {
                continue;
            }
            Function caller = functions.getFunctionContaining(reference.getFromAddress());
            if (caller != null) {
                result.add(caller);
            }
        }
        return result;
    }

    private void decompile(Function function, String reason) {
        println("\n// ===== " + reason + ": " + function.getName() +
                " @ " + function.getEntryPoint() + " =====");
        DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
        if (result != null && result.decompileCompleted()) {
            println(result.getDecompiledFunction().getC());
        } else {
            println("// decompilation failed");
        }
    }

    private void traceNamedFunction(String wantedName) {
        Function target = null;
        FunctionIterator iterator = functions.getFunctions(true);
        while (iterator.hasNext()) {
            Function candidate = iterator.next();
            if (candidate.getName().equals(wantedName)) {
                target = candidate;
                break;
            }
        }
        if (target == null) {
            println("No function named " + wantedName);
            return;
        }

        println("\nTarget " + wantedName + " @ " + target.getEntryPoint());
        Set<Function> directCallers = callersOf(target);
        println("Direct callers: " + directCallers.size());
        for (Function caller : directCallers) {
            decompile(caller, "direct caller of " + wantedName);
        }

        Set<Function> secondLevel = new LinkedHashSet<>();
        for (Function caller : directCallers) {
            secondLevel.addAll(callersOf(caller));
        }
        secondLevel.removeAll(directCallers);
        println("\nSecond-level callers: " + secondLevel.size());
        for (Function caller : secondLevel) {
            decompile(caller, "second-level caller of " + wantedName);
        }
    }

    private void traceInterestingStrings() {
        String[] needles = {
            "HID_Write_Thread",
            "StepUpdateStart",
            "StepUpdateFail",
            "StepUpdateFinish",
            "USB_HID_Updater",
            "NUX DFU"
        };
        DataIterator iterator = currentProgram.getListing().getDefinedData(true);
        while (iterator.hasNext()) {
            Data data = iterator.next();
            Object value = data.getValue();
            if (value == null) {
                continue;
            }
            String rendered = value.toString();
            boolean interesting = false;
            for (String needle : needles) {
                if (rendered.contains(needle)) {
                    interesting = true;
                    break;
                }
            }
            if (!interesting) {
                continue;
            }
            Address address = data.getAddress();
            println("\nString/data " + address + ": " + rendered);
            ReferenceIterator refs = references.getReferencesTo(address);
            Set<Function> containing = new LinkedHashSet<>();
            while (refs.hasNext()) {
                Function function =
                    functions.getFunctionContaining(refs.next().getFromAddress());
                if (function != null) {
                    containing.add(function);
                }
            }
            for (Function function : containing) {
                decompile(function, "references " + rendered);
            }
        }
    }

    @Override
    public void run() throws Exception {
        functions = currentProgram.getFunctionManager();
        references = currentProgram.getReferenceManager();
        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        println("========== NUX updater HID trace ==========");
        traceNamedFunction("hid_write");
        traceNamedFunction("hid_read");
        traceNamedFunction("hid_read_timeout");
        traceInterestingStrings();
        println("========== end NUX updater HID trace ==========");
    }
}
