// Bindings dart:ffi para engine_c_api.h + widgets do editor (Inspetor, Play/Stop).
import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';
import 'package:flutter/material.dart';

typedef _CreateN = Pointer<Void> Function(Pointer<Utf8>);
typedef _CreateD = Pointer<Void> Function(Pointer<Utf8>);
typedef _SetPlayN = Void Function(Pointer<Void>, Int32);
typedef _SetPlayD = void Function(Pointer<Void>, int);
typedef _TransformN = Void Function(Pointer<Void>, Int32, Pointer<Float>, Pointer<Float>, Pointer<Float>);
typedef _TransformD = void Function(Pointer<Void>, int, Pointer<Float>, Pointer<Float>, Pointer<Float>);
typedef _SelectN = Void Function(Pointer<Void>, Int32);
typedef _SelectD = void Function(Pointer<Void>, int);

class EngineApi {
  EngineApi(this.handle)
      : _lib = Platform.isAndroid ? DynamicLibrary.open('libmobile3d.so') : DynamicLibrary.process();

  final Pointer<Void> handle;
  final DynamicLibrary _lib;

  late final _setPlay = _lib.lookupFunction<_SetPlayN, _SetPlayD>('engine_set_play');
  late final _getT = _lib.lookupFunction<_TransformN, _TransformD>('engine_get_transform');
  late final _setT = _lib.lookupFunction<_TransformN, _TransformD>('engine_set_transform');
  late final _select = _lib.lookupFunction<_SelectN, _SelectD>('engine_select');

  void setPlay(bool play) => _setPlay(handle, play ? 1 : 0);
  void select(int id) => _select(handle, id);

  // Buffers nativos reutilizados (sem malloc por chamada)
  late final Pointer<Float> _p = calloc<Float>(3), _r = calloc<Float>(3), _s = calloc<Float>(3);

  ({List<double> pos, List<double> rot, List<double> scale}) getTransform(int id) {
    _getT(handle, id, _p, _r, _s);
    List<double> l(Pointer<Float> p) => [p[0], p[1], p[2]];
    return (pos: l(_p), rot: l(_r), scale: l(_s));
  }

  void setTransform(int id, List<double> pos, List<double> rot, List<double> scale) {
    for (var i = 0; i < 3; i++) {
      _p[i] = pos[i]; _r[i] = rot[i]; _s[i] = scale[i];
    }
    _setT(handle, id, _p, _r, _s);
  }
}

/// Barra superior do editor: Play / Stop instantâneo.
class PlayStopBar extends StatefulWidget {
  const PlayStopBar({super.key, required this.api});
  final EngineApi api;
  @override
  State<PlayStopBar> createState() => _PlayStopBarState();
}

class _PlayStopBarState extends State<PlayStopBar> {
  bool playing = false;
  @override
  Widget build(BuildContext context) => IconButton(
        icon: Icon(playing ? Icons.stop : Icons.play_arrow),
        onPressed: () => setState(() {
          playing = !playing;
          widget.api.setPlay(playing);
        }),
      );
}

/// Inspetor: edita Transform do objeto selecionado em tempo real (cada alteração -> FFI).
class TransformInspector extends StatefulWidget {
  const TransformInspector({super.key, required this.api, required this.objectId});
  final EngineApi api;
  final int objectId;
  @override
  State<TransformInspector> createState() => _TransformInspectorState();
}

class _TransformInspectorState extends State<TransformInspector> {
  late var t = widget.api.getTransform(widget.objectId);

  Widget row(String label, List<double> v, double min, double max, void Function(int, double) onChanged) => Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: Theme.of(context).textTheme.labelMedium),
          for (var i = 0; i < 3; i++)
            Slider(
              value: v[i].clamp(min, max),
              min: min,
              max: max,
              onChanged: (x) => setState(() {
                onChanged(i, x);
                widget.api.setTransform(widget.objectId, t.pos, t.rot, t.scale);
              }),
            ),
        ],
      );

  @override
  Widget build(BuildContext context) => ListView(children: [
        row('Posição', t.pos, -50, 50, (i, x) => t.pos[i] = x),
        row('Rotação (°)', t.rot, -180, 180, (i, x) => t.rot[i] = x),
        row('Escala', t.scale, 0.01, 10, (i, x) => t.scale[i] = x),
      ]);
}
