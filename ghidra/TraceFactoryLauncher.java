// Trace the exact stock Core Deluxe engine copy and handoff routines.
//
// Run against the verified full dump imported at 0x60000000:
//   analyzeHeadless ... -process dump1.bin -noanalysis \
//     -scriptPath ghidra -postScript TraceFactoryLauncher.java

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

public class TraceFactoryLauncher extends GhidraScript {
    private static final long STOCK_MEMCPY = 0x600029f8L;
    private static final long STOCK_INIT_0 = 0x600040b8L;
    private static final long STOCK_INIT_1 = 0x60002e3cL;
    private static final long STOCK_INIT_2 = 0x60003da4L;
    private static final long STOCK_INIT_3 = 0x60004154L;
    private static final long STOCK_INIT_4 = 0x60003f50L;
    private static final long STOCK_PRE_HANDOFF = 0x60003e70L;
    private static final long STOCK_LAUNCHER = 0x60003e78L;
    private static final long STOCK_HANDOFF = 0x60003ff8L;

    private AddressSpace space;
    private Listing listing;
    private Memory memory;
    private FunctionManager functions;
    private ReferenceManager references;
    private DecompInterface decompiler;

    private Address address(long value) {
        return space.getAddress(value & 0xffffffffL);
    }

    private void printWindow(long start, long end) throws Exception {
        println(
            "\n=== instructions 0x" + Long.toHexString(start) +
            "..0x" + Long.toHexString(end) + " ===");
        Address cursor = address(start);
        Address limit = address(end);
        while (cursor.compareTo(limit) < 0) {
            Instruction instruction = listing.getInstructionAt(cursor);
            if (instruction == null) {
                printf(
                    "%s: .hword 0x%04x%n",
                    cursor,
                    memory.getShort(cursor) & 0xffff);
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
    }

    private void reportFunction(long target) {
        Address targetAddress = address(target);
        Function function = functions.getFunctionContaining(targetAddress);
        println(
            "\n=== target " + targetAddress + " function=" +
            (function == null ? "(none)" :
                function.getName() + "@" + function.getEntryPoint()) +
            " ===");

        ReferenceIterator iterator =
            references.getReferencesTo(targetAddress);
        while (iterator.hasNext()) {
            Reference reference = iterator.next();
            Function caller = functions.getFunctionContaining(
                reference.getFromAddress());
            printf(
                "reference %s <- %s  %s  caller=%s%n",
                targetAddress,
                reference.getFromAddress(),
                reference.getReferenceType(),
                caller == null ? "(none)" :
                    caller.getName() + "@" + caller.getEntryPoint());
        }

        if (function == null) {
            return;
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
        memory = currentProgram.getMemory();
        functions = currentProgram.getFunctionManager();
        references = currentProgram.getReferenceManager();
        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        printWindow(0x600029d0L, 0x60002a40L);
        printWindow(0x60002e20L, 0x60002ec0L);
        printWindow(0x60003d80L, 0x60003e20L);
        printWindow(0x60003e40L, 0x60003f30L);
        printWindow(0x60003f30L, 0x60003f70L);
        printWindow(0x60003f70L, 0x60004040L);
        printWindow(0x600040a0L, 0x60004180L);
        reportFunction(STOCK_MEMCPY);
        reportFunction(STOCK_INIT_0);
        reportFunction(STOCK_INIT_1);
        reportFunction(STOCK_INIT_2);
        reportFunction(STOCK_INIT_3);
        reportFunction(STOCK_INIT_4);
        reportFunction(STOCK_PRE_HANDOFF);
        reportFunction(STOCK_LAUNCHER);
        reportFunction(STOCK_HANDOFF);
    }
}
