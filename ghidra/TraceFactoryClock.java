// Trace the stock Core Deluxe clock helpers used immediately before an
// effect engine is copied to ITCM and started.
//
// Run against the verified full dump imported at 0x60000000:
//   analyzeHeadless ... -process dump1.bin -noanalysis \
//     -scriptPath ghidra -postScript TraceFactoryClock.java

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;

public class TraceFactoryClock extends GhidraScript {
    private static final long[] TARGETS = {
        0x60003b70L,
        0x60002664L,
        0x6000267cL,
        0x60002694L,
        0x6000269eL,
        0x60003cbcL,
        0x60003d2aL,
    };

    private AddressSpace space;
    private Listing listing;
    private FunctionManager functions;
    private DecompInterface decompiler;

    private Address address(long value) {
        return space.getAddress(value & 0xffffffffL);
    }

    private void report(long target) throws Exception {
        Address targetAddress = address(target);
        Function function = functions.getFunctionContaining(targetAddress);
        println(
            "\n=== target " + targetAddress + " function=" +
            (function == null ? "(none)" :
                function.getName() + "@" + function.getEntryPoint()) +
            " ===");
        if (function == null) {
            return;
        }

        Address cursor = function.getBody().getMinAddress();
        Address limit = function.getBody().getMaxAddress();
        while (cursor.compareTo(limit) <= 0) {
            Instruction instruction = listing.getInstructionAt(cursor);
            if (instruction == null) {
                cursor = cursor.add(2);
                continue;
            }
            printf(
                "%s: %-8s %s%n",
                cursor,
                instruction.getMnemonicString(),
                instruction);
            cursor = instruction.getMaxAddress().add(1);
        }

        DecompileResults results =
            decompiler.decompileFunction(function, 60, monitor);
        if (!results.decompileCompleted()) {
            println("decompile failed: " + results.getErrorMessage());
            return;
        }
        println(results.getDecompiledFunction().getC());
    }

    public void run() throws Exception {
        space =
            currentProgram.getAddressFactory().getDefaultAddressSpace();
        listing = currentProgram.getListing();
        functions = currentProgram.getFunctionManager();
        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (long target : TARGETS) {
            report(target);
        }
    }
}
