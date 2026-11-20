"""Compose the display-only desktop from the original character and room art."""
from make_pet_room_concepts import opacity_mask, pack, room, unpack

CHARACTER_X = 33
FRAME_SIZE = 1024
BANKS = 2


def build_home_frames(character_frames):
    if not character_frames or len(character_frames) % 512:
        raise ValueError('Expected complete 64x64 character frames')
    characters = [unpack(character_frames[i:i + 512])
                  for i in range(0, len(character_frames), 512)]
    masks = [opacity_mask(character) for character in characters]
    frames = bytearray()
    for night in (False, True):
        background = room(night=night)
        for character, mask in zip(characters, masks):
            image = background.copy()
            image.paste(character, (CHARACTER_X, 0), mask)
            frames.extend(pack(image))
    return bytes(frames)
