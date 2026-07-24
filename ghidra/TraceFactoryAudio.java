// Trace the factory Metal engine's board-level audio initialization.
//
// Run against the verified full dump imported at 0x60000000. This script
// reports literal-pool references to the RT1051 audio/clock/DMA peripherals,
// walks direct callers, and decompiles the resulting engine-local functions.
//
// Dynamic execution of ENG3 corrected an earlier static-only assumption:
// the live audio path is SAI1, not the unused SAI2/SAI3 SDK wrappers.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

import java.util.ArrayDeque;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

public class TraceFactoryAudio extends GhidraScript {
    private static final long ENGINE_START = 0x600c0000L;
    private static final long ENGINE_END = 0x600de000L;

    private static final long[][] TARGETS = {
        {0x40384008L, 1}, // SAI1 TX control-register bank
        {0x40384088L, 2}, // SAI1 RX control-register bank
        {0x40384020L, 3}, // SAI1 TDR0
        {0x403840a0L, 4}, // SAI1 RDR0
        {0x40388000L, 5}, // SAI2, present but not used by ENG3 bring-up
        {0x4038c000L, 6}, // SAI3, present but not used by ENG3 bring-up
        {0x400e8000L, 7}, // DMA0
        {0x400ec000L, 8}, // DMAMUX
        {0x400fc000L, 9}, // CCM
        {0x400d8000L, 10}, // CCM_ANALOG
        {0x401f8000L, 11}, // IOMUXC
        {0x400ac000L, 12}, // IOMUXC_GPR
        {0x2000b91cL, 13}, // Metal audio/PLL config
        {0x2000bd40L, 14}, // RX ping-pong buffers
        {0x2000be40L, 15}, // TX ping-pong buffers
        {0x2000bf40L, 16}, // software TCD ring
    };
    private static final String[] TARGET_NAMES = {
        "unused", "SAI1_TX_ACTIVE", "SAI1_RX_ACTIVE", "SAI1_TDR0",
        "SAI1_RDR0", "SAI2_UNUSED", "SAI3_UNUSED", "DMA0", "DMAMUX",
        "CCM", "CCM_ANALOG", "IOMUXC", "IOMUXC_GPR", "AUDIO_PLL_CONFIG",
        "AUDIO_RX_BUFFERS", "AUDIO_TX_BUFFERS", "AUDIO_TCD_RING"
    };
    private static final long[] KNOWN_AUDIO_WRAPPERS = {
        0x600c7f68L, // complete clock + SAI1 + eDMA initializer
        0x600c8112L, // software TCD ring builder
        0x600c81baL, // DMAMUX/eDMA channel initializer
        0x600c82b4L, // eDMA IRQ/callback dispatcher
    };

    private AddressSpace space;
    private Memory memory;
    private FunctionManager functions;
    private ReferenceManager references;
    private DecompInterface decompiler;

    private Address address(long value) {
        return space.getAddress(value & 0xffffffffL);
    }

    private boolean inEngine(Address value) {
        long offset = value.getOffset();
        return offset >= ENGINE_START && offset < ENGINE_END;
    }

    private String functionName(Function function) {
        if (function == null) {
            return "(no function)";
        }
        return function.getName() + "@" + function.getEntryPoint();
    }

    private void addFunction(
        Map<Long, Function> candidates,
        Function function) {
        if (function != null && inEngine(function.getEntryPoint())) {
            candidates.put(
                function.getEntryPoint().getOffset(),
                function);
        }
    }

    private void findLiteralUsers(
        String name,
        long value,
        Map<Long, Function> candidates)
        throws Exception {
        int literalCount = 0;
        int userCount = 0;
        println("=== " + name + " 0x" + Long.toHexString(value) + " ===");
        for (long offset = ENGINE_START;
             offset <= ENGINE_END - 4;
             offset += 4) {
            Address literal = address(offset);
            long word;
            try {
                word = memory.getInt(literal) & 0xffffffffL;
            } catch (Exception error) {
                continue;
            }
            if (word != value) {
                continue;
            }
            literalCount++;
            ReferenceIterator users = references.getReferencesTo(literal);
            while (users.hasNext()) {
                Reference reference = users.next();
                Address source = reference.getFromAddress();
                if (!inEngine(source)) {
                    continue;
                }
                Function function = functions.getFunctionContaining(source);
                printf(
                    "literal %s <- %s  %s  %s%n",
                    literal,
                    source,
                    reference.getReferenceType(),
                    functionName(function));
                addFunction(candidates, function);
                userCount++;
            }
        }
        println(
            "literal_count=" + literalCount +
            " referenced_users=" + userCount);
    }

    private void addCallers(
        Map<Long, Function> candidates,
        int maxDepth) {
        ArrayDeque<Function> queue = new ArrayDeque<>();
        Map<Long, Integer> depths = new TreeMap<>();
        for (Function function : candidates.values()) {
            queue.add(function);
            depths.put(function.getEntryPoint().getOffset(), 0);
        }
        while (!queue.isEmpty()) {
            Function target = queue.removeFirst();
            int depth = depths.get(target.getEntryPoint().getOffset());
            if (depth >= maxDepth) {
                continue;
            }
            ReferenceIterator callers =
                references.getReferencesTo(target.getEntryPoint());
            while (callers.hasNext()) {
                Reference reference = callers.next();
                if (!reference.getReferenceType().isCall()) {
                    continue;
                }
                Function caller = functions.getFunctionContaining(
                    reference.getFromAddress());
                if (caller == null || !inEngine(caller.getEntryPoint())) {
                    continue;
                }
                long entry = caller.getEntryPoint().getOffset();
                if (!candidates.containsKey(entry)) {
                    candidates.put(entry, caller);
                    depths.put(entry, depth + 1);
                    queue.addLast(caller);
                }
            }
        }
    }

    private void printCallers(Function target) {
        Set<String> callers = new TreeSet<>();
        ReferenceIterator iterator =
            references.getReferencesTo(target.getEntryPoint());
        while (iterator.hasNext()) {
            Reference reference = iterator.next();
            if (!reference.getReferenceType().isCall()) {
                continue;
            }
            callers.add(
                functionName(
                    functions.getFunctionContaining(
                        reference.getFromAddress())));
        }
        println("direct_callers=" + callers);
    }

    private void decompile(Function function) {
        println(
            "\n----- " + functionName(function) +
            " size=" + function.getBody().getNumAddresses() + " -----");
        printCallers(function);
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
        memory = currentProgram.getMemory();
        functions = currentProgram.getFunctionManager();
        references = currentProgram.getReferenceManager();
        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        Map<Long, Function> candidates = new TreeMap<>();
        for (long[] target : TARGETS) {
            findLiteralUsers(
                TARGET_NAMES[(int) target[1]],
                target[0],
                candidates);
        }
        for (long wrapper : KNOWN_AUDIO_WRAPPERS) {
            addFunction(
                candidates,
                functions.getFunctionContaining(address(wrapper)));
        }
        addCallers(candidates, 3);

        println("\n=== Candidate audio function set ===");
        for (Function function : candidates.values()) {
            println(functionName(function));
        }
        for (Function function : candidates.values()) {
            decompile(function);
        }
        decompiler.dispose();
    }
}
