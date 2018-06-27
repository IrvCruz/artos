#ifndef LIBARTOS_DEF_H
#define LIBARTOS_DEF_H

#define ARTOS_RES_OK 0
#define ARTOS_RES_INVALID_HANDLE -1
#define ARTOS_RES_DIRECTORY_NOT_FOUND -2
#define ARTOS_RES_FILE_NOT_FOUND -3
#define ARTOS_RES_FILE_ACCESS_DENIED -4
#define ARTOS_RES_ABORTED -5
#define ARTOS_RES_INDEX_OUT_OF_BOUNDS -6
#define ARTOS_RES_INVALID_IMG_DATA -7
#define ARTOS_RES_BUFFER_TOO_SMALL -8
#define ARTOS_RES_INTERNAL_ERROR -999

#define ARTOS_DETECT_RES_INVALID_IMG_DATA -101
#define ARTOS_DETECT_RES_INVALID_MODEL_FILE -102
#define ARTOS_DETECT_RES_INVALID_MODEL_LIST_FILE -103
#define ARTOS_DETECT_RES_NO_MODELS -104
#define ARTOS_DETECT_RES_INVALID_IMAGE -105
#define ARTOS_DETECT_RES_NO_IMAGES -106
#define ARTOS_DETECT_RES_NO_RESULTS -107
#define ARTOS_DETECT_RES_INVALID_ANNOTATIONS -108
#define ARTOS_DETECT_RES_TOO_MANY_MODELS -109
#define ARTOS_DETECT_RES_INVALID_FEATURES -110
#define ARTOS_DETECT_RES_MIXED_FEATURES -111

#define ARTOS_LEARN_RES_FAILED -201
#define ARTOS_LEARN_RES_INVALID_BG_FILE -202
#define ARTOS_LEARN_RES_INVALID_IMG_DATA -203
#define ARTOS_LEARN_RES_NO_SAMPLES -204
#define ARTOS_LEARN_RES_MODEL_NOT_LEARNED -205
#define ARTOS_LEARN_RES_FEATURE_EXTRACTOR_NOT_READY -206

#define ARTOS_IMGREPO_RES_INVALID_REPOSITORY -301
#define ARTOS_IMGREPO_RES_SYNSET_NOT_FOUND -302
#define ARTOS_IMGREPO_RES_EXTRACTION_FAILED -303

#define ARTOS_SETTINGS_RES_UNKNOWN_FEATURE_EXTRACTOR -401
#define ARTOS_SETTINGS_RES_UNKNOWN_PARAMETER -402
#define ARTOS_SETTINGS_RES_INVALID_PARAMETER_VALUE -403


#define ARTOS_THOPT_NONE 0
#define ARTOS_THOPT_OVERLAPPING 1
#define ARTOS_THOPT_LOOCV 2


#define ARTOS_PARAM_TYPE_INT 0
#define ARTOS_PARAM_TYPE_SCALAR 1
#define ARTOS_PARAM_TYPE_STRING 2


#define IMAGENET_IMAGE_DIR "Images"
#define IMAGENET_ANNOTATION_DIR "Annotation"

#endif