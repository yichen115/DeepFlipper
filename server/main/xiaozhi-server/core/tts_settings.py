"""The two speech backends exposed by the lightweight admin page."""
from pathlib import Path

LOCAL_TTS = {'type': 'local_melo', 'model_dir': 'models/vits-melo-tts-zh_en',
             'speaker_id': 0, 'speed': 1.0, 'num_threads': 4, 'output_dir': 'tmp/'}


def local_model_files(root, config):
    models = (Path(root) / 'models').resolve()
    directory = (Path(root) / config.get('model_dir', LOCAL_TTS['model_dir'])).resolve()
    if not directory.is_relative_to(models):
        raise ValueError('Local TTS model must be inside models/')
    required = ('model.onnx', 'tokens.txt', 'lexicon.txt', 'dict')
    if any(not (directory / name).exists() for name in required):
        raise FileNotFoundError('Run python scripts/download_models.py --tts-only first')
    return directory
