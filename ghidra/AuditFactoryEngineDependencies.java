// Audit Ghidra references from the preserved factory Metal engine into the
// boot region, factory state, and proposed open-metadata sector.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class AuditFactoryEngineDependencies extends GhidraScript {
    private static final String[] ENGINE_NAMES = {
        "Delay", "Reverb", "Modulation", "Metal"
    };
    private static final long[] ENGINE_STARTS = {
        0x60060000L,
        0x60080000L,
        0x600a0000L,
        0x600c0000L
    };
    private static final long ENGINE_COPY_SIZE = 0x0001e000L;
    private static final long BOOT_START = 0x60002000L;
    private static final long BOOT_END = 0x60020000L;
    private static final long FACTORY_STATE_START = 0x60020000L;
    private static final long FACTORY_STATE_END = 0x60030000L;
    private static final long PROPOSED_METADATA_START = 0x603f0000L;
    private static final long PROPOSED_METADATA_END = 0x60400000L;

    private String targetKind(long target) {
        if (target >= BOOT_START && target < BOOT_END) {
            return "boot";
        }
        if (target >= FACTORY_STATE_START &&
            target < FACTORY_STATE_END) {
            return "factory-state";
        }
        if (target >= PROPOSED_METADATA_START &&
            target < PROPOSED_METADATA_END) {
            return "proposed-metadata";
        }
        return null;
    }

    private void auditEngine(
        String name,
        long start,
        long end) {
        AddressSpace space =
            currentProgram.getAddressFactory().getDefaultAddressSpace();
        Address engineStart = space.getAddress(start);
        Address engineEnd = space.getAddress(end);
        ReferenceManager references =
            currentProgram.getReferenceManager();
        AddressIterator sources =
            references.getReferenceSourceIterator(engineStart, true);
        int count = 0;
        int factoryStateCount = 0;
        int unsafeCount = 0;
        int sourceCount = 0;
        int instructionCount = 0;

        InstructionIterator instructions =
            currentProgram.getListing().getInstructions(
                new AddressSet(
                    engineStart,
                    engineEnd.subtract(1)),
                true);
        while (instructions.hasNext()) {
            instructions.next();
            instructionCount++;
        }

        println("=== " + name + " engine dependency audit ===");
        while (sources.hasNext()) {
            Address source = sources.next();
            if (source.compareTo(engineEnd) >= 0) {
                break;
            }
            sourceCount++;
            for (Reference reference :
                references.getReferencesFrom(source)) {
                long target = reference.getToAddress().getOffset();
                String targetKind = targetKind(target);
                if (targetKind != null) {
                    printf(
                        "%s -> %s  %-8s %s%n",
                        source,
                        reference.getToAddress(),
                        targetKind,
                        reference.getReferenceType());
                    count++;
                    if (targetKind.equals("factory-state")) {
                        factoryStateCount++;
                    } else {
                        unsafeCount++;
                    }
                }
            }
        }
        println("instructions=" + instructionCount);
        println("reference_sources=" + sourceCount);
        println("count=" + count);
        if (factoryStateCount != 7) {
            throw new RuntimeException(
                name + " factory-state reference set changed: " +
                factoryStateCount);
        }
        if (unsafeCount != 0) {
            throw new RuntimeException(
                name + " references open boot or proposed metadata: " +
                unsafeCount);
        }
    }

    public void run() throws Exception {
        for (int index = 0; index < ENGINE_NAMES.length; index++) {
            auditEngine(
                ENGINE_NAMES[index],
                ENGINE_STARTS[index],
                ENGINE_STARTS[index] + ENGINE_COPY_SIZE);
        }
    }
}
