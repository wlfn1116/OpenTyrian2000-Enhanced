#include "MIDIProcessor.h"
#include "MIDIContainer.h"
#include "midiproc.h"

EXPORT size_t MIDPROC_process_and_serialize_to_smf(const uint8_t * data, size_t data_size, const char * file_extension, uint8_t ** data_out)
{
    // convert the data to a vector
    std::vector<uint8_t> data_vector(data, data + data_size);
    MIDIContainer * container = new MIDIContainer();
    if (!MIDIProcessor::Process(data_vector, file_extension, *container))
    {
        delete container;
        return 0;
    }
    // create a std::vector to hold the serialized data on the heap
    std::vector<uint8_t> * serialized_container = new std::vector<uint8_t>();

    container->SerializeAsSMF(*serialized_container);
    size_t size = serialized_container->size();
    // copy the serialized data to the output buffer
    *data_out = new uint8_t[serialized_container->size()];
    std::copy(serialized_container->begin(), serialized_container->end(), *data_out);
    delete serialized_container;
    // free the memory
    delete container;
    return size;
}

EXPORT HMIDIContainer MIDPROC_Container_Create()
{
    MIDIContainer * container = new MIDIContainer();
    return static_cast<HMIDIContainer>(container);
}

EXPORT void MIDPROC_Container_Delete(HMIDIContainer processor)
{
    MIDIContainer * container = static_cast<MIDIContainer *>(processor);
    delete container;
}

EXPORT bool MIDPROC_Process(const uint8_t * data, size_t data_size, const char * file_extension, HMIDIContainer container)
{
    std::vector<uint8_t> data_vector(data, data + data_size);
    if (!MIDIProcessor::Process(data_vector, file_extension, *container)){
        return false;
    }
    return true;
}

EXPORT void MIDPROC_Container_SerializeAsSMF(HMIDIContainer container, uint8_t ** data_out, size_t * data_out_size)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    std::vector<uint8_t> * serialized_container = new std::vector<uint8_t>();
    c->SerializeAsSMF(*serialized_container);
    *data_out = new uint8_t[serialized_container->size()];
    std::copy(serialized_container->begin(), serialized_container->end(), *data_out);
    *data_out_size = serialized_container->size();
    delete serialized_container;
}

EXPORT void MIDPROC_Container_SerializeAsSMFLoop(HMIDIContainer container, uint8_t ** data_out, size_t * data_out_size)
{
    *data_out = nullptr;
    *data_out_size = 0;

    MIDIContainer * c = static_cast<MIDIContainer *>(container);

    uint32_t LoopBegin = c->GetLoopBeginTimestamp(0, false);

    if ((LoopBegin == ~0u) || (LoopBegin == 0))
        return; // no loop, or the loop is the whole song: whole-file repeat suffices

    uint32_t EndTimestamp = c->GetDuration(0, false);

    if (EndTimestamp <= LoopBegin)
        return;

    uint32_t LoopLength = EndTimestamp - LoopBegin;

    MIDIContainer Loop;

    Loop.Initialize(c->GetFormat(), c->GetTimeDivision());

    static const uint8_t EndOfTrackData[2] = { StatusCodes::MetaData, MetaDataTypes::EndOfTrack };
    static const uint8_t AllNotesOffData[2] = { 123, 0 }; // CC 123, All Notes Off

    size_t TrackIndex = 0;

    for (const MIDITrack & Track : *c)
    {
        MIDITrack NewTrack;

        // When the loop file wraps around, notes still sounding from its end would
        // hang forever (their note-offs exist only before the loop point in the
        // original song), so release every channel's notes first at the seam.
        if (TrackIndex == 0)
        {
            for (uint32_t Channel = 0; Channel < 16; ++Channel)
                NewTrack.AddEvent(MIDIEvent(0, MIDIEvent::ControlChange, Channel, AllNotesOffData, 2));
        }

        for (const MIDIEvent & Event : Track)
        {
            bool IsMeta = (Event.Type == MIDIEvent::Extended) && (Event.Data.size() >= 2) && (Event.Data[0] == StatusCodes::MetaData);

            // End-of-track is re-added below; the loop markers have served their purpose.
            if (IsMeta && ((Event.Data[1] == MetaDataTypes::EndOfTrack) || (Event.Data[1] == MetaDataTypes::Marker)))
                continue;

            MIDIEvent NewEvent(Event);

            if (Event.Timestamp >= LoopBegin)
            {
                NewEvent.Timestamp = Event.Timestamp - LoopBegin;
            }
            else
            {
                // Pre-loop event: keep the ones that establish synth state, squashed
                // to the start of the file in their original order (last one wins);
                // notes and note-bound aftertouch don't carry state across the loop.
                switch (Event.Type)
                {
                    case MIDIEvent::ControlChange:
                    case MIDIEvent::ProgramChange:
                    case MIDIEvent::ChannelPressureAftertouch:
                    case MIDIEvent::PitchBendChange:
                        break;

                    case MIDIEvent::Extended: // SysEx, or a meta event
                        if (IsMeta && (Event.Data[1] != MetaDataTypes::SetTempo))
                            continue;
                        break;

                    default:
                        continue;
                }

                NewEvent.Timestamp = 0;
            }

            NewTrack.AddEvent(NewEvent);
        }

        NewTrack.AddEvent(MIDIEvent(LoopLength, MIDIEvent::Extended, 0, EndOfTrackData, 2));

        Loop.AddTrack(NewTrack);

        ++TrackIndex;
    }

    std::vector<uint8_t> Serialized;

    Loop.SerializeAsSMF(Serialized);

    if (Serialized.empty())
        return;

    *data_out = new uint8_t[Serialized.size()];
    std::copy(Serialized.begin(), Serialized.end(), *data_out);
    *data_out_size = Serialized.size();
}

EXPORT uint32_t MIDPROC_Container_GetFormat(HMIDIContainer container)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    return c->GetFormat();
}

EXPORT uint32_t MIDPROC_Container_GetTrackCount(HMIDIContainer container)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    return c->GetTrackCount();
}

EXPORT uint32_t MIDPROC_Container_GetChannelCount(HMIDIContainer container, size_t subSongIndex)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    return c->GetChannelCount(subSongIndex);
}

EXPORT uint32_t MIDPROC_Container_GetLoopBeginTimestamp(HMIDIContainer container, size_t subSongIndex, bool ms)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    return c->GetLoopBeginTimestamp(subSongIndex, ms);
}

EXPORT uint32_t MIDPROC_Container_GetLoopEndTimestamp(HMIDIContainer container, size_t subSongIndex, bool ms)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    return c->GetLoopEndTimestamp(subSongIndex, ms);
}

EXPORT uint32_t MIDPROC_Container_GetDuration(HMIDIContainer container, size_t subSongIndex, bool ms)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    return c->GetDuration(subSongIndex, ms);
}

EXPORT size_t MIDPROC_Container_GetSubSongCount(HMIDIContainer container)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    return c->GetSubSongCount();
}

EXPORT size_t MIDPROC_Container_GetSubSong(HMIDIContainer container, size_t index)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    return c->GetSubSong(index);
}

EXPORT void MIDPROC_Container_PromoteToType1(HMIDIContainer container)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    c->PromoteToType1();
}

EXPORT void MIDPROC_Container_DetectLoops(HMIDIContainer container, bool detectXMILoops, bool detectMarkerLoops, bool detectRPGMakerLoops, bool detectTouhouLoops)
{
    MIDIContainer * c = static_cast<MIDIContainer *>(container);
    c->DetectLoops(detectXMILoops, detectMarkerLoops, detectRPGMakerLoops, detectTouhouLoops);
}
