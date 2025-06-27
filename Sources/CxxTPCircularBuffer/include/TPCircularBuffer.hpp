//
//  TPCircularBuffer.hpp
//  Music Streamer (iOS)
//
//  Created by Charles Kiorpes on 6/26/25.
//

#ifndef TPCircularBuffer_hpp
#define TPCircularBuffer_hpp

#include <AudioToolbox/AudioToolbox.h>
#include <stdbool.h>
#include <assert.h>
#include <string>
#include <atomic>
typedef std::atomic_int atomicInt;

#define kTPCircularBufferCopyAll UINT32_MAX

typedef struct {
    void             *buffer;
    uint32_t           length;
    uint32_t           tail;
    uint32_t           head;
    volatile atomicInt fillCount;
    bool              atomic;
} TPCircularBufferDesc;

typedef struct {
    AudioTimeStamp timestamp;
    UInt32 totalLength;
    AudioBufferList bufferList;
} TPCircularBufferABLBlockHeader;

class TPCircularBuffer {
public:

    TPCircularBuffer(uint32_t length) {
        this->_initBuffer(length, sizeof(TPCircularBufferDesc));
    };

    ~TPCircularBuffer() {
        this->Clear();
        this->Cleanup();
    };

    /*!
    * Initialise buffer
    *
    *  Note that the length is advisory only: Because of the way the
    *  memory mirroring technique works, the true buffer length will
    *  be multiples of the device page size (e.g. 4096 bytes)
    *
    *  If you intend to use the AudioBufferList utilities, you should
    *  always allocate a bit more space than you need for pure audio
    *  data, so there's room for the metadata. How much extra is required
    *  depends on how many AudioBufferList structures are used, which is
    *  a function of how many audio frames each buffer holds. A good rule
    *  of thumb is to add 15%, or at least another 2048 bytes or so.
    *
    * @param length Length of buffer
    */
    bool InitBuffer(uint32_t length) {
        return this->_initBuffer(length, sizeof(TPCircularBufferDesc));
    }

    /*!
    * Cleanup buffer
    *
    *  Releases buffer resources.
    */
    void Cleanup();

    /*!
    * Clear buffer
    *
    *  Resets buffer to original, empty state.
    *
    *  This is safe for use by consumer while producer is accessing
    *  buffer.
    */
    void Clear();

    /*!
    * Set the atomicity
    *
    *  If you set the atomiticy to false using this method, the buffer will
    *  not use atomic operations. This can be used to give the compiler a little
    *  more optimisation opportunities when the buffer is only used on one thread.
    *
    *  Important note: Only set this to false if you know what you're doing!
    *
    *  The default value is true (the buffer will use atomic operations)
    *
    * @param atomic Whether the buffer is atomic (default true)
    */
    void SetAtomic(bool atomic);

    // Reading (consuming)

    /*!
    * Access end of buffer
    *
    *  This gives you a pointer to the end of the buffer, ready
    *  for reading, and the number of available bytes to read.
    *
    * @param availableBytes On output, the number of bytes ready for reading
    * @return Pointer to the first bytes ready for reading, or NULL if buffer is empty
    */
    void* Tail(uint32_t* availableBytes);

    /*!
    * Consume bytes in buffer
    *
    *  This frees up the just-read bytes, ready for writing again.
    *
    * @param amount Number of bytes to consume
    */
    void Consume(uint32_t amount);

    /*!
    * Access front of buffer
    *
    *  This gives you a pointer to the front of the buffer, ready
    *  for writing, and the number of available bytes to write.
    *
    * @param availableBytes On output, the number of bytes ready for writing
    * @return Pointer to the first bytes ready for writing, or NULL if buffer is full
    */
    void* Head(uint32_t* availableBytes);
        
    // Writing (producing)

    /*!
    * Produce bytes in buffer
    *
    *  This marks the given section of the buffer ready for reading.
    *
    * @param amount Number of bytes to produce
    */
    void Produce(uint32_t amount);

    /*!
    * Helper routine to copy bytes to buffer
    *
    *  This copies the given bytes to the buffer, and marks them ready for reading.
    *
    * @param src Source buffer
    * @param len Number of bytes in source buffer
    * @return true if bytes copied, false if there was insufficient space
    */
    bool ProduceBytes(const void* src, uint32_t len);





    // MARK: TPCircularBuffer+AudioBufferList

    

    
    /*!
    * Prepare an empty buffer list, stored on the circular buffer
    *
    * @param numberOfBuffers   The number of buffers to be contained within the buffer list
    * @param bytesPerBuffer    The number of bytes to store for each buffer
    * @param timestamp         The timestamp associated with the buffer, or NULL. Note that you can also pass a timestamp into TPCircularBufferProduceAudioBufferList, to set it there instead.
    * @return The empty buffer list, or NULL if circular buffer has insufficient space
    */
    AudioBufferList *PrepareEmptyAudioBufferList(UInt32 numberOfBuffers, UInt32 bytesPerBuffer, const AudioTimeStamp *timestamp);

    /*!
    * Prepare an empty buffer list, stored on the circular buffer, using an audio description to automatically configure buffer
    *
    * @param audioFormat       The kind of audio that will be stored
    * @param frameCount        The number of frames that will be stored
    * @param timestamp         The timestamp associated with the buffer, or NULL. Note that you can also pass a timestamp into TPCircularBufferProduceAudioBufferList, to set it there instead.
    * @return The empty buffer list, or NULL if circular buffer has insufficient space
    */
    AudioBufferList *PrepareEmptyAudioBufferListWithAudioFormat(const AudioStreamBasicDescription *audioFormat, UInt32 frameCount, const AudioTimeStamp *timestamp);

    /*!
    * Mark next audio buffer list as ready for reading
    *
    *  This marks the audio buffer list prepared using TPCircularBufferPrepareEmptyAudioBufferList
    *  as ready for reading. You must not call this function without first calling
    *  TPCircularBufferPrepareEmptyAudioBufferList.
    *
    * @param inTimestamp       The timestamp associated with the buffer, or NULL to leave as-is. Note that you can also pass a timestamp into TPCircularBufferPrepareEmptyAudioBufferList, to set it there instead.
    */
    void ProduceAudioBufferList(const AudioTimeStamp *inTimestamp);

    /*!
    * Copy the audio buffer list onto the buffer
    *
    * @param bufferList        Buffer list containing audio to copy to buffer
    * @param timestamp         The timestamp associated with the buffer, or NULL
    * @param frames            Length of audio in frames. Specify kTPCircularBufferCopyAll to copy the whole buffer (audioFormat can be NULL, in this case)
    * @param audioFormat       The AudioStreamBasicDescription describing the audio, or NULL if you specify kTPCircularBufferCopyAll to the `frames` argument
    * @return YES if buffer list was successfully copied; NO if there was insufficient space
    */
    bool CopyAudioBufferList(const AudioBufferList *bufferList, const AudioTimeStamp *timestamp, UInt32 frames, const AudioStreamBasicDescription *audioFormat);

    /*!
    * Get a pointer to the next stored buffer list
    *
    * @param outTimestamp      On output, if not NULL, the timestamp corresponding to the buffer
    * @return Pointer to the next buffer list in the buffer
    */
    AudioBufferList *NextBufferList(AudioTimeStamp *outTimestamp);

    /*!
    * Get a pointer to the next stored buffer list after the given one
    *
    * @param bufferList        Preceding buffer list
    * @param outTimestamp      On output, if not NULL, the timestamp corresponding to the buffer
    * @return Pointer to the next buffer list in the buffer, or NULL
    */
    AudioBufferList *NextBufferListAfter(const AudioBufferList *bufferList, AudioTimeStamp *outTimestamp);

    /*!
    * Consume the next buffer list
    */
    void ConsumeNextBufferList();

    /*!
    * Consume a portion of the next buffer list
    *
    *  This will also increment the sample time and host time portions of the timestamp of
    *  the buffer list, if present.
    *
    * @param framesToConsume The number of frames to consume from the buffer list
    * @param audioFormat The AudioStreamBasicDescription describing the audio
    */
    void ConsumeNextBufferListPartial(UInt32 framesToConsume, const AudioStreamBasicDescription *audioFormat);

    /*!
    * Consume a certain number of frames from the buffer, possibly from multiple queued buffer lists
    *
    *  Copies the given number of frames from the buffer into outputBufferList, of the
    *  given audio description, then consumes the audio buffers. If an audio buffer has
    *  not been entirely consumed, then updates the queued buffer list structure to point
    *  to the unconsumed data only.
    *
    * @param ioLengthInFrames  On input, the number of frames in the given audio format to consume; on output, the number of frames provided
    * @param outputBufferList  The buffer list to copy audio to, or NULL to discard audio. If not NULL, the structure must be initialised properly, and the mData pointers must not be NULL.
    * @param outTimestamp      On output, if not NULL, the timestamp corresponding to the first audio frame returned
    * @param audioFormat       The format of the audio stored in the buffer
    */
    void DequeueBufferListFrames(UInt32 *ioLengthInFrames, const AudioBufferList *outputBufferList, AudioTimeStamp *outTimestamp, const AudioStreamBasicDescription *audioFormat);

    /*!
    * Determine how many frames of audio are buffered
    *
    *  Given the provided audio format, determines the frame count of all queued buffers
    *
    *  Note: This function should only be used on the consumer thread, not the producer thread.
    *
    * @param outTimestamp      On output, if not NULL, the timestamp corresponding to the first audio frame
    * @param audioFormat       The format of the audio stored in the buffer
    * @return The number of frames in the given audio format that are in the buffer
    */
    UInt32 Peek(AudioTimeStamp *outTimestamp, const AudioStreamBasicDescription *audioFormat);

    /*!
    * Determine how many contiguous frames of audio are buffered
    *
    *  Given the provided audio format, determines the frame count of all queued buffers that are contiguous,
    *  given their corresponding timestamps (sample time).
    *
    *  Note: This function should only be used on the consumer thread, not the producer thread.
    *
    * @param outTimestamp      On output, if not NULL, the timestamp corresponding to the first audio frame
    * @param audioFormat       The format of the audio stored in the buffer
    * @param contiguousToleranceSampleTime The number of samples of discrepancy to tolerate
    * @return The number of frames in the given audio format that are in the buffer
    */
    UInt32 PeekContiguous(AudioTimeStamp *outTimestamp, const AudioStreamBasicDescription *audioFormat, UInt32 contiguousToleranceSampleTime);
        
    /*!
    * Determine how many contiguous frames of audio are buffered, with wrap around
    *
    *  Like TPCircularBufferPeekContiguous, determines how many contiguous frames are buffered,
    *  but considers audio that wraps around a region of a given length as also contiguous. This
    *  is good for audio that loops.
    *
    *  Note: This function should only be used on the consumer thread, not the producer thread.
    *
    * @param outTimestamp      On output, if not NULL, the timestamp corresponding to the first audio frame
    * @param audioFormat       The format of the audio stored in the buffer
    * @param contiguousToleranceSampleTime The number of samples of discrepancy to tolerate
    * @param wrapPoint         The point around which the audio may wrap and still be considered contiguous, or 0 to disable
    * @return The number of frames in the given audio format that are in the buffer
    */
    UInt32 PeekContiguousWrapped(AudioTimeStamp *outTimestamp, const AudioStreamBasicDescription *audioFormat, UInt32 contiguousToleranceSampleTime, UInt32 wrapPoint);

    /*!
    * Determine how many much space there is in the buffer
    *
    *  Given the provided audio format, determines the number of frames of audio that can be buffered.
    *
    *  Note: This function should only be used on the producer thread, not the consumer thread.
    *
    * @param audioFormat       The format of the audio stored in the buffer
    * @return The number of frames in the given audio format that can be stored in the buffer
    */
    UInt32 GetAvailableSpace(const AudioStreamBasicDescription *audioFormat);

private:
    TPCircularBufferDesc bufferDesc;
    
    bool _initBuffer(uint32_t length, size_t structSize);
};

#endif /* TPCircularBuffer_hpp */
