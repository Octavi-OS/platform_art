/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "assembler.h"

#include <algorithm>
#include <vector>

#include "assembler_arm.h"
#include "assembler_x86.h"
#include "globals.h"
#include "memory_region.h"

namespace art {

static byte* NewContents(size_t capacity) {
  byte* result = new byte[capacity];
#if defined(DEBUG)
  // Initialize the buffer with kBreakPointInstruction to force a break
  // point if we ever execute an uninitialized part of the code buffer.
  Assembler::InitializeMemoryWithBreakpoints(result, capacity);
#endif
  return result;
}


#if defined(DEBUG)
AssemblerBuffer::EnsureCapacity::EnsureCapacity(AssemblerBuffer* buffer) {
  if (buffer->cursor() >= buffer->limit()) buffer->ExtendCapacity();
  // In debug mode, we save the assembler buffer along with the gap
  // size before we start emitting to the buffer. This allows us to
  // check that any single generated instruction doesn't overflow the
  // limit implied by the minimum gap size.
  buffer_ = buffer;
  gap_ = ComputeGap();
  // Make sure that extending the capacity leaves a big enough gap
  // for any kind of instruction.
  CHECK_GE(gap_, kMinimumGap);
  // Mark the buffer as having ensured the capacity.
  CHECK(!buffer->HasEnsuredCapacity());  // Cannot nest.
  buffer->has_ensured_capacity_ = true;
}


AssemblerBuffer::EnsureCapacity::~EnsureCapacity() {
  // Unmark the buffer, so we cannot emit after this.
  buffer_->has_ensured_capacity_ = false;
  // Make sure the generated instruction doesn't take up more
  // space than the minimum gap.
  int delta = gap_ - ComputeGap();
  CHECK_LE(delta, kMinimumGap);
}
#endif


AssemblerBuffer::AssemblerBuffer() {
  static const size_t kInitialBufferCapacity = 4 * KB;
  contents_ = NewContents(kInitialBufferCapacity);
  cursor_ = contents_;
  limit_ = ComputeLimit(contents_, kInitialBufferCapacity);
  fixup_ = NULL;
  slow_path_ = NULL;
#if defined(DEBUG)
  has_ensured_capacity_ = false;
  fixups_processed_ = false;
#endif

  // Verify internal state.
  CHECK_EQ(Capacity(), kInitialBufferCapacity);
  CHECK_EQ(Size(), 0U);
}


AssemblerBuffer::~AssemblerBuffer() {
  delete[] contents_;
}


void AssemblerBuffer::ProcessFixups(const MemoryRegion& region) {
  AssemblerFixup* fixup = fixup_;
  while (fixup != NULL) {
    fixup->Process(region, fixup->position());
    fixup = fixup->previous();
  }
}


void AssemblerBuffer::FinalizeInstructions(const MemoryRegion& instructions) {
  // Copy the instructions from the buffer.
  MemoryRegion from(reinterpret_cast<void*>(contents()), Size());
  instructions.CopyFrom(0, from);
  // Process fixups in the instructions.
  ProcessFixups(instructions);
#if defined(DEBUG)
  fixups_processed_ = true;
#endif
}


void AssemblerBuffer::ExtendCapacity() {
  size_t old_size = Size();
  size_t old_capacity = Capacity();
  size_t new_capacity = std::min(old_capacity * 2, old_capacity + 1 * MB);

  // Allocate the new data area and copy contents of the old one to it.
  byte* new_contents = NewContents(new_capacity);
  memmove(reinterpret_cast<void*>(new_contents),
          reinterpret_cast<void*>(contents_),
          old_size);

  // Compute the relocation delta and switch to the new contents area.
  ptrdiff_t delta = new_contents - contents_;
  contents_ = new_contents;

  // Update the cursor and recompute the limit.
  cursor_ += delta;
  limit_ = ComputeLimit(new_contents, new_capacity);

  // Verify internal state.
  CHECK_EQ(Capacity(), new_capacity);
  CHECK_EQ(Size(), old_size);
}


Assembler* Assembler::Create(InstructionSet instruction_set) {
  if (instruction_set == kX86) {
    return new x86::X86Assembler();
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    return new arm::ArmAssembler();
  }
}

}  // namespace art
