#ifndef PLAYER_CODEC_HOOKS_H
#define PLAYER_CODEC_HOOKS_H
struct H264Context;
struct AVFrame;
struct AVFrame *player_codec_frame_acquire(struct H264Context *context);
void player_codec_frame_release(struct H264Context *context, struct AVFrame *frame);
int player_codec_frame_publish(struct H264Context *context, struct AVFrame *frame);
#endif
