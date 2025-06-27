//
//  TPCircularBuffer.c
//  Circular/Ring buffer implementation
//
//  https://github.com/michaeltyson/TPCircularBuffer
//
//  Created by Michael Tyson on 10/12/2011.
//
//  Copyright (C) 2012-2013 A Tasty Pixel
//
//  This software is provided 'as-is', without any express or implied
//  warranty.  In no event will the authors be held liable for any damages
//  arising from the use of this software.
//
//  Permission is granted to anyone to use this software for any purpose,
//  including commercial applications, and to alter it and redistribute it
//  freely, subject to the following restrictions:
//
//  1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software
//     in a product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//
//  2. Altered source versions must be plainly marked as such, and must not be
//     misrepresented as being the original software.
//
//  3. This notice may not be removed or altered from any source distribution.
//

#include "TPCircularBuffer.hpp"
#include <mach/mach.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#import <mach/mach_time.h>

static double __secondsToHostTicks = 0.0;

#define reportResult(result,operation) (_reportResult((result),(operation),strrchr(__FILE__, '/')+1,__LINE__))
static inline bool _reportResult(kern_return_t result, const char *operation, const char* file, int line) {
    if ( result != ERR_SUCCESS ) {
        printf("%s:%d: %s: %s\n", file, line, operation, mach_error_string(result));
        return false;
    }
    return true;
}

bool TPCircularBuffer::_initBuffer(uint32_t length, size_t structSize) {
    
    assert(length > 0);
    
    if ( structSize != sizeof(TPCircularBufferDesc) ) {
        fprintf(stderr, "TPCircularBuffer: Header version mismatch. Check for old versions of TPCircularBuffer in your project\n");
        abort();
    }
    
    // Keep trying until we get our buffer, needed to handle race conditions
    int retries = 3;
    while ( true ) {

        this->bufferDesc.length = (uint32_t)round_page(length);    // We need whole page sizes

        // Temporarily allocate twice the length, so we have the contiguous address space to
        // support a second instance of the buffer directly after
        vm_address_t bufferAddress;
        kern_return_t result = vm_allocate(mach_task_self(),
                                           &bufferAddress,
                                           this->bufferDesc.length * 2,
                                           VM_FLAGS_ANYWHERE); // allocate anywhere it'll fit
        if ( result != ERR_SUCCESS ) {
            if ( retries-- == 0 ) {
                reportResult(result, "Buffer allocation");
                return false;
            }
            // Try again if we fail
            continue;
        }
        
        // Now replace the second half of the allocation with a virtual copy of the first half. Deallocate the second half...
        result = vm_deallocate(mach_task_self(),
                               bufferAddress + this->bufferDesc.length,
                               this->bufferDesc.length);
        if ( result != ERR_SUCCESS ) {
            if ( retries-- == 0 ) {
                reportResult(result, "Buffer deallocation");
                return false;
            }
            // If this fails somehow, deallocate the whole region and try again
            vm_deallocate(mach_task_self(), bufferAddress, this->bufferDesc.length);
            continue;
        }
        
        // Re-map the buffer to the address space immediately after the buffer
        vm_address_t virtualAddress = bufferAddress + this->bufferDesc.length;
        vm_prot_t cur_prot, max_prot;
        result = vm_remap(mach_task_self(),
                          &virtualAddress,   // mirror target
                          this->bufferDesc.length,    // size of mirror
                          0,                 // auto alignment
                          0,                 // force remapping to virtualAddress
                          mach_task_self(),  // same task
                          bufferAddress,     // mirror source
                          0,                 // MAP READ-WRITE, NOT COPY
                          &cur_prot,         // unused protection struct
                          &max_prot,         // unused protection struct
                          VM_INHERIT_DEFAULT);
        if ( result != ERR_SUCCESS ) {
            if ( retries-- == 0 ) {
                reportResult(result, "Remap buffer memory");
                return false;
            }
            // If this remap failed, we hit a race condition, so deallocate and try again
            vm_deallocate(mach_task_self(), bufferAddress, this->bufferDesc.length);
            continue;
        }
        
        if ( virtualAddress != bufferAddress + this->bufferDesc.length ) {
            // If the memory is not contiguous, clean up both allocated buffers and try again
            if ( retries-- == 0 ) {
                printf("Couldn't map buffer memory to end of buffer\n");
                return false;
            }

            vm_deallocate(mach_task_self(), virtualAddress, this->bufferDesc.length);
            vm_deallocate(mach_task_self(), bufferAddress, this->bufferDesc.length);
            continue;
        }
        
        this->bufferDesc.buffer = (void*)bufferAddress;
        this->bufferDesc.fillCount = 0;
        this->bufferDesc.head = this->bufferDesc.tail = 0;
        this->bufferDesc.atomic = true;
        
        return true;
    }
    return false;
}

void TPCircularBuffer::Cleanup() {
    vm_deallocate(mach_task_self(), (vm_address_t)this->bufferDesc.buffer, this->bufferDesc.length * 2);
    std::memset(&this->bufferDesc, 0, sizeof(TPCircularBufferDesc));
}

void TPCircularBuffer::Clear() {
    uint32_t fillCount;
    if ( this->Tail(&fillCount) ) {
        this->Consume(fillCount);
    }
}

void TPCircularBuffer::SetAtomic(bool atomic) {
    this->bufferDesc.atomic = atomic;
}

inline void* TPCircularBuffer::Tail(uint32_t* availableBytes) {
    *availableBytes = this->bufferDesc.fillCount;
    if ( *availableBytes == 0 ) return NULL;
    return (void*)((char*)this->bufferDesc.buffer + this->bufferDesc.tail);
}

inline void TPCircularBuffer::Consume(uint32_t amount) {
    this->bufferDesc.tail = (this->bufferDesc.tail + amount) % this->bufferDesc.length;
    if ( this->bufferDesc.atomic ) {
        std::atomic_fetch_add(&this->bufferDesc.fillCount, -(int)amount);
    } else {
        this->bufferDesc.fillCount -= amount;
    }
    assert(this->bufferDesc.fillCount >= 0);
}

inline void* TPCircularBuffer::Head(uint32_t* availableBytes) {
    *availableBytes = (this->bufferDesc.length - this->bufferDesc.fillCount);
    if ( *availableBytes == 0 ) return NULL;
    return (void*)((char*)this->bufferDesc.buffer + this->bufferDesc.head);
}

inline void TPCircularBuffer::Produce(uint32_t amount) {
    this->bufferDesc.head = (this->bufferDesc.head + amount) % this->bufferDesc.length;
    if ( this->bufferDesc.atomic ) {
        std::atomic_fetch_add(&this->bufferDesc.fillCount, (int)amount);
    } else {
        this->bufferDesc.fillCount += amount;
    }
    assert(this->bufferDesc.fillCount <= this->bufferDesc.length);
}

inline bool TPCircularBuffer::ProduceBytes(const void* src, uint32_t len) {
    uint32_t space;
    void *ptr = this->Head(&space);
    if ( space < len ) return false;
    std::memcpy(ptr, src, len);
    this->Produce(len);
    return true;
}

static inline unsigned long align16byte(unsigned long val) {
    if ( val & (16-1) ) {
        return val + (16 - (val & (16-1)));
    }
    return val;
}

static inline unsigned long min(unsigned long a, unsigned long b) {
    return a > b ? b : a;
}

AudioBufferList *TPCircularBuffer::PrepareEmptyAudioBufferList(UInt32 numberOfBuffers, UInt32 bytesPerBuffer, const AudioTimeStamp *inTimestamp) {
    uint32_t availableBytes;
    TPCircularBufferABLBlockHeader *block = (TPCircularBufferABLBlockHeader*)this->Head(&availableBytes);
    if ( !block || availableBytes < sizeof(TPCircularBufferABLBlockHeader)+((numberOfBuffers-1)*sizeof(AudioBuffer))+(numberOfBuffers*bytesPerBuffer) ) return NULL;
    
    #ifdef DEBUG
    assert(!((unsigned long)block & 0xF) /* Beware unaligned accesses */);
    #endif
    
    if ( inTimestamp ) {
        std::memcpy(&block->timestamp, inTimestamp, sizeof(AudioTimeStamp));
    } else {
        std::memset(&block->timestamp, 0, sizeof(AudioTimeStamp));
    }
    
    std::memset(&block->bufferList, 0, sizeof(AudioBufferList)+((numberOfBuffers-1)*sizeof(AudioBuffer)));
    block->bufferList.mNumberBuffers = numberOfBuffers;
    
    char *dataPtr = (char*)&block->bufferList + sizeof(AudioBufferList)+((numberOfBuffers-1)*sizeof(AudioBuffer));
    for ( UInt32 i=0; i<numberOfBuffers; i++ ) {
        // Find the next 16-byte aligned memory area
        dataPtr = (char*)align16byte((unsigned long)dataPtr);
        
        if ( (UInt32)((dataPtr + bytesPerBuffer) - (char*)block) > availableBytes ) {
            return NULL;
        }
        
        block->bufferList.mBuffers[i].mData = dataPtr;
        block->bufferList.mBuffers[i].mDataByteSize = bytesPerBuffer;
        block->bufferList.mBuffers[i].mNumberChannels = 1;
        
        dataPtr += bytesPerBuffer;
    }
    
    // Make sure whole buffer (including timestamp and length value) is 16-byte aligned in length
    block->totalLength = (UInt32)align16byte((unsigned long)(dataPtr - (char*)block));
    if ( block->totalLength > availableBytes ) {
        return NULL;
    }
    
    return &block->bufferList;
}

AudioBufferList *TPCircularBuffer::PrepareEmptyAudioBufferListWithAudioFormat(const AudioStreamBasicDescription *audioFormat, UInt32 frameCount, const AudioTimeStamp *timestamp) {
    return TPCircularBuffer::PrepareEmptyAudioBufferList(
        (audioFormat->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? audioFormat->mChannelsPerFrame : 1,
        audioFormat->mBytesPerFrame * frameCount,
        timestamp
    );
}

void TPCircularBuffer::ProduceAudioBufferList(const AudioTimeStamp *inTimestamp) {
    uint32_t availableBytes;
    TPCircularBufferABLBlockHeader *block = (TPCircularBufferABLBlockHeader*)this->Head(&availableBytes);
    
    assert(block);
    
    #ifdef DEBUG
    assert(!((unsigned long)block & 0xF) /* Beware unaligned accesses */);
    #endif
    
    assert(block->bufferList.mBuffers[0].mDataByteSize > 0);
    
    if ( inTimestamp ) {
        std::memcpy(&block->timestamp, inTimestamp, sizeof(AudioTimeStamp));
    }
    
    UInt32 calculatedLength = (UInt32)(((char*)block->bufferList.mBuffers[block->bufferList.mNumberBuffers-1].mData + block->bufferList.mBuffers[block->bufferList.mNumberBuffers-1].mDataByteSize) - (char*)block);

    // Make sure whole buffer (including timestamp and length value) is 16-byte aligned in length
    calculatedLength = (UInt32)align16byte(calculatedLength);
    
    assert(calculatedLength <= block->totalLength && calculatedLength <= availableBytes);
    
    block->totalLength = calculatedLength;
    
    this->Produce(block->totalLength);
}

bool TPCircularBuffer::CopyAudioBufferList(const AudioBufferList *inBufferList, const AudioTimeStamp *inTimestamp, UInt32 frames, const AudioStreamBasicDescription *audioDescription) {
    if ( frames == 0 ) return true;
    
    UInt32 byteCount = inBufferList->mBuffers[0].mDataByteSize;
    if ( frames != kTPCircularBufferCopyAll ) {
        byteCount = frames * audioDescription->mBytesPerFrame;
        assert(byteCount <= inBufferList->mBuffers[0].mDataByteSize);
    }
    
    if ( byteCount == 0 ) return true;
    
    AudioBufferList *bufferList = this->PrepareEmptyAudioBufferList(inBufferList->mNumberBuffers, byteCount, inTimestamp);
    if ( !bufferList ) return false;
    
    for ( UInt32 i=0; i<bufferList->mNumberBuffers; i++ ) {
        memcpy(bufferList->mBuffers[i].mData, inBufferList->mBuffers[i].mData, byteCount);
    }
    
    this->ProduceAudioBufferList(NULL);
    
    return true;
}

inline AudioBufferList *TPCircularBuffer::NextBufferList(AudioTimeStamp *outTimestamp) {
    uint32_t dontcare; // Length of segment is contained within buffer list, so we can ignore this
    TPCircularBufferABLBlockHeader *block = (TPCircularBufferABLBlockHeader*)this->Tail(&dontcare);
    if ( !block ) {
        if ( outTimestamp ) {
            std::memset(outTimestamp, 0, sizeof(AudioTimeStamp));
        }
        return NULL;
    }
    if ( outTimestamp ) {
        std::memcpy(outTimestamp, &block->timestamp, sizeof(AudioTimeStamp));
    }
    return &block->bufferList;
}

AudioBufferList *TPCircularBuffer::NextBufferListAfter(const AudioBufferList *bufferList, AudioTimeStamp *outTimestamp) {
    uint32_t availableBytes;
    void *tail = this->Tail(&availableBytes);
    void *end = (char*)tail + availableBytes;
    assert((void*)bufferList > (void*)tail && (void*)bufferList < end);
    
    TPCircularBufferABLBlockHeader *originalBlock = (TPCircularBufferABLBlockHeader*)((char*)bufferList - offsetof(TPCircularBufferABLBlockHeader, bufferList));
    
    #ifdef DEBUG
    assert(!((unsigned long)originalBlock & 0xF) /* Beware unaligned accesses */);
    #endif
    
    TPCircularBufferABLBlockHeader *nextBlock = (TPCircularBufferABLBlockHeader*)((char*)originalBlock + originalBlock->totalLength);
    if ( (void*)nextBlock >= end ) return NULL;
    
    #ifdef DEBUG
    assert(!((unsigned long)nextBlock & 0xF) /* Beware unaligned accesses */);
    #endif
    
    if ( outTimestamp ) {
        std::memcpy(outTimestamp, &nextBlock->timestamp, sizeof(AudioTimeStamp));
    }
    
    return &nextBlock->bufferList;
}

inline void TPCircularBuffer::ConsumeNextBufferList() {
    uint32_t dontcare;
    TPCircularBufferABLBlockHeader *block = (TPCircularBufferABLBlockHeader*)this->Tail(&dontcare);
    if ( !block ) return;
    this->Consume(block->totalLength);
}

void TPCircularBuffer::ConsumeNextBufferListPartial(UInt32 framesToConsume, const AudioStreamBasicDescription *audioFormat) {
    
    uint32_t dontcare;
    TPCircularBufferABLBlockHeader *block = (TPCircularBufferABLBlockHeader*)this->Tail(&dontcare);
    if ( !block ) return;
    
    #ifdef DEBUG
    assert(!((unsigned long)block & 0xF)); // Beware unaligned accesses
    #endif
    
    UInt32 bytesToConsume = (UInt32)min(framesToConsume * audioFormat->mBytesPerFrame, block->bufferList.mBuffers[0].mDataByteSize);
    
    if ( bytesToConsume == block->bufferList.mBuffers[0].mDataByteSize ) {
        this->ConsumeNextBufferList();
        return;
    }
    
    for ( UInt32 i=0; i<block->bufferList.mNumberBuffers; i++ ) {
        assert(bytesToConsume <= block->bufferList.mBuffers[i].mDataByteSize);
        
        block->bufferList.mBuffers[i].mData = (char*)block->bufferList.mBuffers[i].mData + bytesToConsume;
        block->bufferList.mBuffers[i].mDataByteSize -= bytesToConsume;
    }
    
    if ( block->timestamp.mFlags & kAudioTimeStampSampleTimeValid ) {
        block->timestamp.mSampleTime += framesToConsume;
    }
    if ( block->timestamp.mFlags & kAudioTimeStampHostTimeValid ) {
        if ( __secondsToHostTicks == 0.0 ) {
            mach_timebase_info_data_t tinfo;
            mach_timebase_info(&tinfo);
            __secondsToHostTicks = 1.0 / (((double)tinfo.numer / tinfo.denom) * 1.0e-9);
        }

        block->timestamp.mHostTime += (UInt64)(((double)framesToConsume / audioFormat->mSampleRate) * __secondsToHostTicks);
    }
    
    // Reposition block forward, just before the audio data, ensuring 16-byte alignment
    TPCircularBufferABLBlockHeader *newBlock = (TPCircularBufferABLBlockHeader*)(((unsigned long)block + bytesToConsume) & ~0xFul);
    std::memmove(newBlock, block, sizeof(TPCircularBufferABLBlockHeader) + (block->bufferList.mNumberBuffers-1)*sizeof(AudioBuffer));
    UInt32 bytesFreed = (UInt32)((intptr_t)newBlock - (intptr_t)block);
    newBlock->totalLength -= bytesFreed;
    this->Consume(bytesFreed);
}

void TPCircularBuffer::DequeueBufferListFrames(UInt32 *ioLengthInFrames, const AudioBufferList *outputBufferList, AudioTimeStamp *outTimestamp, const AudioStreamBasicDescription *audioFormat) {
    bool hasTimestamp = false;
    UInt32 bytesToGo = *ioLengthInFrames * audioFormat->mBytesPerFrame;
    UInt32 bytesCopied = 0;
    while ( bytesToGo > 0 ) {
        AudioBufferList *bufferList = this->NextBufferList(!hasTimestamp ? outTimestamp : NULL);
        if ( !bufferList ) break;
        
        hasTimestamp = true;
        UInt32 bytesToCopy = (UInt32)min(bytesToGo, bufferList->mBuffers[0].mDataByteSize);
        
        if ( outputBufferList ) {
            for ( UInt32 i=0; i<outputBufferList->mNumberBuffers; i++ ) {
                assert(bytesCopied + bytesToCopy <= outputBufferList->mBuffers[i].mDataByteSize);
                std::memcpy((char*)outputBufferList->mBuffers[i].mData + bytesCopied, bufferList->mBuffers[i].mData, bytesToCopy);
            }
        }
        
        this->ConsumeNextBufferListPartial(bytesToCopy/audioFormat->mBytesPerFrame, audioFormat);
        
        bytesToGo -= bytesToCopy;
        bytesCopied += bytesToCopy;
    }
    
    *ioLengthInFrames -= bytesToGo / audioFormat->mBytesPerFrame;
}

UInt32 TPCircularBuffer::PeekContiguousWrapped(AudioTimeStamp *outTimestamp, const AudioStreamBasicDescription *audioFormat, UInt32 contiguousToleranceSampleTime, UInt32 wrapPoint) {
    uint32_t availableBytes;
    TPCircularBufferABLBlockHeader *block = (TPCircularBufferABLBlockHeader*)this->Tail(&availableBytes);
    if ( !block ) return 0;
    
    #ifdef DEBUG
    assert(!((unsigned long)block & 0xF) /* Beware unaligned accesses */);
    #endif
    
    if ( outTimestamp ) {
        std::memcpy(outTimestamp, &block->timestamp, sizeof(AudioTimeStamp));
    }
    
    void *end = (char*)block + availableBytes;
    
    UInt32 byteCount = 0;
    
    while ( 1 ) {
        byteCount += block->bufferList.mBuffers[0].mDataByteSize;
        TPCircularBufferABLBlockHeader *nextBlock = (TPCircularBufferABLBlockHeader*)((char*)block + block->totalLength);
        if ( (void*)nextBlock >= end ) {
            break;
        }
        
        if ( contiguousToleranceSampleTime != UINT32_MAX ) {
            UInt32 frames = block->bufferList.mBuffers[0].mDataByteSize / audioFormat->mBytesPerFrame;
            Float64 nextTime = block->timestamp.mSampleTime + frames;
            if ( wrapPoint && nextTime > wrapPoint ) nextTime = fmod(nextTime, wrapPoint);
            Float64 diff = fabs(nextBlock->timestamp.mSampleTime - nextTime);
            if ( diff > contiguousToleranceSampleTime && (!wrapPoint || fabs(diff-wrapPoint) > contiguousToleranceSampleTime) ) {
                break;
            }
        }
        
        #ifdef DEBUG
        assert(!((unsigned long)nextBlock & 0xF) /* Beware unaligned accesses */);
        #endif
        
        block = nextBlock;
    }
    
    return byteCount / audioFormat->mBytesPerFrame;
}

UInt32 TPCircularBuffer::Peek(AudioTimeStamp *outTimestamp, const AudioStreamBasicDescription *audioFormat) {
    return this->PeekContiguousWrapped(outTimestamp, audioFormat, UINT32_MAX, 0);
}

UInt32 TPCircularBuffer::PeekContiguous(AudioTimeStamp *outTimestamp, const AudioStreamBasicDescription *audioFormat, UInt32 contiguousToleranceSampleTime) {
    return this->PeekContiguousWrapped(outTimestamp, audioFormat, contiguousToleranceSampleTime, 0);
}

UInt32 TPCircularBuffer::GetAvailableSpace(const AudioStreamBasicDescription *audioFormat) {
    // Look at buffer head; make sure there's space for the block metadata
    uint32_t availableBytes;
    TPCircularBufferABLBlockHeader *block = (TPCircularBufferABLBlockHeader*)this->Head(&availableBytes);
    if ( !block ) return 0;
    
    #ifdef DEBUG
    assert(!((unsigned long)block & 0xF) /* Beware unaligned accesses */);
    #endif
    
    // Now find out how much 16-byte aligned audio we can store in the space available
    UInt32 numberOfBuffers = audioFormat->mFormatFlags & kAudioFormatFlagIsNonInterleaved ? audioFormat->mChannelsPerFrame : 1;
    char * endOfBuffer = (char*)block + availableBytes;
    char * dataPtr = (char*)align16byte((unsigned long)(&block->bufferList + sizeof(AudioBufferList)+((numberOfBuffers-1)*sizeof(AudioBuffer))));
    if ( dataPtr >= endOfBuffer ) return 0;
    UInt32 availableAudioBytes = (UInt32)(endOfBuffer - dataPtr);
    
    UInt32 availableAudioBytesPerBuffer = availableAudioBytes / numberOfBuffers;
    availableAudioBytesPerBuffer -= (availableAudioBytesPerBuffer % (16-1));
    
    return availableAudioBytesPerBuffer > 0 ? availableAudioBytesPerBuffer / audioFormat->mBytesPerFrame : 0;
}

