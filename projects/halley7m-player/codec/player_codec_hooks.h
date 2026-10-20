#ifndef PLAYER_CODEC_HOOKS_H
#define PLAYER_CODEC_HOOKS_H
struct H264Context;
struct AVFrame;
struct H264Picture;
struct H264SliceContext;
struct AVFrame *player_codec_frame_acquire(struct H264Context *context);
void player_codec_frame_release(struct H264Context *context, struct AVFrame *frame);
int player_codec_frame_publish(struct H264Context *context, struct AVFrame *frame);
int player_codec_picture_acquire(struct H264Context *context, struct H264Picture *picture,
                                 struct AVFrame *frame);
void player_codec_picture_release(struct H264Context *context, struct H264Picture *picture);
void player_codec_prepare_weights(struct H264Context *context, const struct H264SliceContext *slice);
#endif
