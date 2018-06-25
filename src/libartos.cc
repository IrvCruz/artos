#include "libartos.h"
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <utility>
#include "ModelEvaluator.h"
#include "ImageNetModelLearner.h"
#include "ImageRepository.h"
#include "StationaryBackground.h"
#include "Scene.h"
#include "sysutils.h"
using namespace std;
using namespace ARTOS;


vector<ModelEvaluator*> detectors;
vector<ImageNetModelLearner*> learners;

bool is_valid_detector_handle(const unsigned int detector);
bool is_valid_learner_handle(const unsigned int learner);


//-------------------------------------------------------------------
//---------------------------- Detecting ----------------------------
//-------------------------------------------------------------------


map< unsigned int, vector<Sample*> > eval_positive_samples;
map< unsigned int, vector<JPEGImage> > eval_negative_samples;

int detect_jpeg(const unsigned int detector, const JPEGImage & img, FlatDetection * detection_buf, unsigned int * detection_buf_size);
void write_results_to_buffer(const vector<Detection> & detections, FlatDetection * detection_buf, unsigned int * detection_buf_size);


unsigned int create_detector(const double overlap, const int interval, const bool debug)
{
    ModelEvaluator * newDetector = 0;
    try
    {
        newDetector = new ModelEvaluator(overlap, overlap, interval, debug);
        detectors.push_back(newDetector);
        return detectors.size(); // return handle of the new detector
    }
    catch (exception e)
    {
        if (newDetector != 0)
            delete newDetector;
        return 0; // allocation error
    }
}

void destroy_detector(const unsigned int detector)
{
    if (is_valid_detector_handle(detector))
        try
        {
            delete detectors[detector - 1];
            detectors[detector - 1] = NULL;
            for (vector<Sample*>::iterator sample = eval_positive_samples[detector].begin(); sample != eval_positive_samples[detector].end(); sample++)
                delete *sample;
            eval_positive_samples.erase(detector);
            eval_negative_samples.erase(detector);
        }
        catch (exception e) { }
}
    
int add_model(const unsigned int detector, const char * classname, const char * modelfile, const double threshold, const char * synset_id)
{
    if (is_valid_detector_handle(detector))
        return detectors[detector - 1]->addModel(classname, modelfile, threshold, (synset_id != NULL) ? synset_id : "");
    else
        return ARTOS_RES_INVALID_HANDLE;
}

int add_models(const unsigned int detector, const char * modellistfile)
{
    if (is_valid_detector_handle(detector))
        return detectors[detector - 1]->addModels(modellistfile);
    else
        return ARTOS_RES_INVALID_HANDLE;
}

int add_model_from_learner(const unsigned int detector, const char * classname, const unsigned int learner, const double threshold, const char * synset_id)
{
    if (is_valid_detector_handle(detector) && is_valid_learner_handle(learner))
    {
        ModelLearnerBase * learner_obj = learners[learner - 1];
        Mixture mix(learner_obj->getFeatureExtractor());
        for (size_t i = 0; i < learner_obj->getModels().size(); i++)
            mix.addModel(Model(learner_obj->getModels()[i], -1 * learner_obj->getThresholds()[i]));
        return detectors[detector - 1]->addModel(classname, move(mix), threshold, (synset_id != NULL) ? synset_id : "");
    }
    else
        return ARTOS_RES_INVALID_HANDLE;
}

int num_feature_extractors_in_detector(const unsigned int detector)
{
    if (is_valid_detector_handle(detector))
        return detectors[detector - 1]->differentFeatureExtractors();
    else
        return -1;
}

int detect_file_jpeg(const unsigned int detector,
                             const char * imagefile,
                             FlatDetection * detection_buf, unsigned int * detection_buf_size)
{
    return detect_jpeg(detector, JPEGImage(imagefile), detection_buf, detection_buf_size);
}

int detect_raw(const unsigned int detector,
                       const unsigned char * img_data, const unsigned int img_width, const unsigned int img_height, const bool grayscale,
                       FlatDetection * detection_buf, unsigned int * detection_buf_size)
{
    return detect_jpeg(detector, JPEGImage(img_width, img_height, (grayscale) ? 1 : 3, img_data), detection_buf, detection_buf_size);
}


int detect_jpeg(const unsigned int detector, const JPEGImage & img, FlatDetection * detection_buf, unsigned int * detection_buf_size)
{
    if (is_valid_detector_handle(detector))
    {
        if (img.empty())
            return ARTOS_DETECT_RES_INVALID_IMG_DATA;
        vector<Detection> detections;
        int result;
        if (*detection_buf_size == 1)
        {
            Detection detection;
            result = detectors[detector - 1]->detectMax(img, detection);
            detections.push_back(move(detection));
        }
        else
            result = detectors[detector - 1]->detect(img, detections);
        if (result == ARTOS_RES_OK)
        {
            sort(detections.begin(), detections.end());
            write_results_to_buffer(detections, detection_buf, detection_buf_size);
        }
        else
            *detection_buf_size = 0;
        return result;
    }
    else
        return ARTOS_RES_INVALID_HANDLE;
}

void write_results_to_buffer(const vector<Detection> & detections, FlatDetection * detection_buf, unsigned int * detection_buf_size)
{
    vector<Detection>::const_iterator dit; // iterator over detection results
    unsigned int bi; // index for accessing the detection buffer
    for (dit = detections.begin(), bi = 0; dit < detections.end() && bi < *detection_buf_size; dit++, bi++, detection_buf++)
    {
        // Write detection data to the detection buffer
        memset(detection_buf->classname, 0, sizeof(detection_buf->classname));
        dit->classname.copy(detection_buf->classname, sizeof(detection_buf->classname) - 1);
        memset(detection_buf->synset_id, 0, sizeof(detection_buf->synset_id));
        dit->synsetId.copy(detection_buf->synset_id, sizeof(detection_buf->synset_id) - 1);
        detection_buf->score = static_cast<float>(dit->score);
        detection_buf->left = dit->left();
        detection_buf->top = dit->top();
        detection_buf->right = dit->right() + 1;
        detection_buf->bottom = dit->bottom() + 1;
    }
    *detection_buf_size = bi; // store number of detections written to the buffer
}

bool is_valid_detector_handle(const unsigned int detector)
{
    return (detector > 0 && detector <= detectors.size() && detectors[detector - 1] != NULL);
}


//------------------------------------------------------------------
//---------------------------- Training ----------------------------
//------------------------------------------------------------------


typedef struct {
    overall_progress_cb_t cb;
    unsigned int overall_step;
    unsigned int overall_steps_total;
    bool aborted;
} progress_params;

bool populate_progress(unsigned int cur, unsigned int total, void * data)
{
    progress_params * params = reinterpret_cast<progress_params*>(data);
    if (!params->aborted)
        params->aborted = !params->cb(params->overall_step, params->overall_steps_total, cur, total);
    return !params->aborted;
}


int learn_imagenet(const char * repo_directory, const char * synset_id, const char * bg_file, const char * modelfile,
                            const bool add, const unsigned int max_aspect_clusters, const unsigned int max_who_clusters,
                            const unsigned int th_opt_num_positive, const unsigned int th_opt_num_negative, const unsigned int th_opt_mode,
                            overall_progress_cb_t progress_cb, const bool debug)
{
    // Check repository
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    // Search synset
    ImageRepository repo = ImageRepository(repo_directory);
    Synset synset = repo.getSynset(synset_id);
    if (synset.id.empty())
        return ARTOS_IMGREPO_RES_SYNSET_NOT_FOUND;
    // Load background statistics
    StationaryBackground bg(bg_file);
    if (bg.empty())
        return ARTOS_LEARN_RES_INVALID_BG_FILE;
    
    // Setup some stuff for progress callback
    progress_params progParams;
    progParams.cb = progress_cb;
    progParams.overall_step = 0;
    progParams.overall_steps_total = (th_opt_mode == ARTOS_THOPT_NONE) ? 2 : 3;
    progParams.aborted = false;
    if (progress_cb != NULL)
        progress_cb(0, progParams.overall_steps_total, 0, 0);
    
    // Learn model
    ImageNetModelLearner learner(bg, repo, nullptr, (th_opt_mode == ARTOS_THOPT_LOOCV), debug);
    if (learner.addPositiveSamplesFromSynset(synset) == 0)
        return ARTOS_IMGREPO_RES_EXTRACTION_FAILED;
    progParams.overall_step++;
    int res;
    try {
        res = learner.learn(max_aspect_clusters, max_who_clusters, (progress_cb != NULL) ? &populate_progress : NULL, reinterpret_cast<void*>(&progParams));
    } catch (const UseBeforeSetupException & e) {
        res = ARTOS_LEARN_RES_FEATURE_EXTRACTOR_NOT_READY;
    }
    if (res != ARTOS_RES_OK)
        return res;
    if (th_opt_mode != ARTOS_THOPT_NONE)
    {
        progParams.overall_step++;
        learner.optimizeThreshold(th_opt_num_positive, th_opt_num_negative, 1.0f,
                                (progress_cb != NULL) ? &populate_progress : NULL, reinterpret_cast<void*>(&progParams));
    }
    
    // Save model
    if (!learner.save(modelfile, add))
        return ARTOS_RES_FILE_ACCESS_DENIED;
    
    if (progress_cb != NULL)
        progress_cb(progParams.overall_steps_total, progParams.overall_steps_total, 0, 0);
    return ARTOS_RES_OK;
}

int learn_files_jpeg(const char ** imagefiles, const unsigned int num_imagefiles, const FlatBoundingBox * bounding_boxes,
                            const char * bg_file, const char * modelfile, const bool add,
                            const unsigned int max_aspect_clusters, const unsigned int max_who_clusters,
                            const unsigned int th_opt_mode,
                            overall_progress_cb_t progress_cb, const bool debug)
{
    // Load background statistics
    StationaryBackground bg(bg_file);
    if (bg.empty())
        return ARTOS_LEARN_RES_INVALID_BG_FILE;
    
    // Setup some stuff for progress callback
    progress_params progParams;
    progParams.cb = progress_cb;
    progParams.overall_step = 0;
    progParams.overall_steps_total = (th_opt_mode == ARTOS_THOPT_NONE) ? 2 : 3;
    progParams.aborted = false;
    if (progress_cb != NULL)
        progress_cb(0, progParams.overall_steps_total, 0, 0);
    
    // Add samples
    ModelLearner learner(bg, nullptr, (th_opt_mode == ARTOS_THOPT_LOOCV), debug);
    Rectangle bbox; // empty bounding box
    const FlatBoundingBox * flat_bbox;
    for (unsigned int i = 0; i < num_imagefiles; i++)
    {
        JPEGImage img(imagefiles[i]);
        if (!img.empty())
        {
            if (bounding_boxes != NULL)
            {
                flat_bbox = bounding_boxes + i;
                bbox = Rectangle(flat_bbox->left, flat_bbox->top, flat_bbox->width, flat_bbox->height);
            }
            learner.addPositiveSample(img, bbox);
        }
    }
    progParams.overall_step++;
    
    // Learn model
    int res;
    try {
        res = learner.learn(max_aspect_clusters, max_who_clusters, (progress_cb != NULL) ? &populate_progress : NULL, reinterpret_cast<void*>(&progParams));
    } catch (const UseBeforeSetupException & e) {
        res = ARTOS_LEARN_RES_FEATURE_EXTRACTOR_NOT_READY;
    }
    if (res != ARTOS_RES_OK)
        return res;
    if (th_opt_mode != ARTOS_THOPT_NONE)
    {
        progParams.overall_step++;
        if (progress_cb == NULL)
            learner.optimizeThreshold();
        else
            learner.optimizeThreshold(0, NULL, 1.0f, &populate_progress, reinterpret_cast<void*>(&progParams));
    }
    
    // Save model
    if (!learner.save(modelfile, add))
        return ARTOS_RES_FILE_ACCESS_DENIED;
    
    if (progress_cb != NULL)
        progress_cb(progParams.overall_steps_total, progParams.overall_steps_total, 0, 0);
    return ARTOS_RES_OK;
}


int learner_add_jpeg(const unsigned int learner, const JPEGImage & img, const FlatBoundingBox * bboxes, const unsigned int num_bboxes);
bool progress_proxy(unsigned int current, unsigned int total, void * data);

unsigned int create_learner(const char * bg_file, const char * repo_directory, const bool th_opt_loocv, const bool debug)
{
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        repo_directory = "";
    ImageNetModelLearner * newLearner = new ImageNetModelLearner(bg_file, repo_directory, nullptr, th_opt_loocv, debug);
    if (newLearner->getBackground().empty())
    {
        delete newLearner;
        return 0;
    }
    learners.push_back(newLearner);
    return learners.size(); // return handle of the new detector
}

void destroy_learner(const unsigned int learner)
{
    if (is_valid_learner_handle(learner))
        try
        {
            delete learners[learner - 1];
            learners[learner - 1] = NULL;
        }
        catch (exception e) { }
}

int learner_add_synset(const unsigned int learner, const char * synset_id, const unsigned int max_samples)
{
    if (!is_valid_learner_handle(learner))
        return ARTOS_RES_INVALID_HANDLE;
    ImageRepository repo = learners[learner - 1]->getRepository();
    if (repo.getRepoDirectory().empty())
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    // Search synset
    Synset synset = repo.getSynset(synset_id);
    if (synset.id.empty())
        return ARTOS_IMGREPO_RES_SYNSET_NOT_FOUND;
    if (learners[learner - 1]->addPositiveSamplesFromSynset(synset, max_samples) == 0)
        return ARTOS_IMGREPO_RES_EXTRACTION_FAILED;
    return ARTOS_RES_OK;
}

int learner_add_file_jpeg(const unsigned int learner, const char * imagefile,
                          const FlatBoundingBox * bboxes, const unsigned int num_bboxes)
{
    return learner_add_jpeg(learner, JPEGImage(imagefile), bboxes, num_bboxes);
}

int learner_add_raw(const unsigned int learner,
                    const unsigned char * img_data, const unsigned int img_width, const unsigned int img_height, const bool grayscale,
                    const FlatBoundingBox * bboxes, const unsigned int num_bboxes)
{
    return learner_add_jpeg(learner, JPEGImage(img_width, img_height, (grayscale) ? 1 : 3, img_data), bboxes, num_bboxes);
}

int learner_run(const unsigned int learner, const unsigned int max_aspect_clusters, const unsigned int max_who_clusters, progress_cb_t progress_cb)
{
    if (!is_valid_learner_handle(learner))
        return ARTOS_RES_INVALID_HANDLE;
    ImageNetModelLearner * l = learners[learner - 1];
    if (l->getNumSamples() == 0)
        return ARTOS_LEARN_RES_NO_SAMPLES;
    ProgressCallback progressCB = (progress_cb != NULL) ? &progress_proxy : NULL;
    void * cbData = (progress_cb != NULL) ? reinterpret_cast<void*>(progress_cb) : NULL;
    int res;
    try {
        res = l->learn(max_aspect_clusters, max_who_clusters, progressCB, cbData);
    } catch (const UseBeforeSetupException & e) {
        res = ARTOS_LEARN_RES_FEATURE_EXTRACTOR_NOT_READY;
    }
    return res;
}

int learner_optimize_th(const unsigned int learner, const unsigned int max_positive, const unsigned int num_negative, progress_cb_t progress_cb)
{
    if (!is_valid_learner_handle(learner))
        return ARTOS_RES_INVALID_HANDLE;
    ImageNetModelLearner * l = learners[learner - 1];
    if (l->getModels().empty())
        return ARTOS_LEARN_RES_MODEL_NOT_LEARNED;
    if (num_negative > 0 && l->getRepository().getRepoDirectory().empty())
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    ProgressCallback progressCB = (progress_cb != NULL) ? &progress_proxy : NULL;
    void * cbData = (progress_cb != NULL) ? reinterpret_cast<void*>(progress_cb) : NULL;
    l->optimizeThreshold(max_positive, num_negative, 1.0f, progressCB, cbData);
    return (progressCB != NULL && cbData == NULL) ? ARTOS_RES_ABORTED : ARTOS_RES_OK;
}

int learner_save(const unsigned int learner, const char * modelfile, const bool add)
{
    if (!is_valid_learner_handle(learner))
        return ARTOS_RES_INVALID_HANDLE;
    ImageNetModelLearner * l = learners[learner - 1];
    if (l->getModels().empty())
        return ARTOS_LEARN_RES_MODEL_NOT_LEARNED;
    return (l->save(modelfile, add)) ? ARTOS_RES_OK : ARTOS_RES_FILE_ACCESS_DENIED;
}

int learner_reset(const unsigned int learner)
{
    if (!is_valid_learner_handle(learner))
        return ARTOS_RES_INVALID_HANDLE;
    learners[learner - 1]->reset();
    return ARTOS_RES_OK;
}


bool is_valid_learner_handle(const unsigned int learner)
{
    return (learner > 0 && learner <= learners.size() && learners[learner - 1] != NULL);
}

int learner_add_jpeg(const unsigned int learner, const JPEGImage & img, const FlatBoundingBox * bboxes, const unsigned int num_bboxes)
{
    if (!is_valid_learner_handle(learner))
        return ARTOS_RES_INVALID_HANDLE;
    if (img.empty())
        return ARTOS_LEARN_RES_INVALID_IMG_DATA;
    vector<Rectangle> _bboxes;
    if (bboxes != NULL)
        for (const FlatBoundingBox * flat_bbox = bboxes; flat_bbox < bboxes + num_bboxes; flat_bbox++)
            _bboxes.push_back(Rectangle(flat_bbox->left, flat_bbox->top, flat_bbox->width, flat_bbox->height));
    learners[learner - 1]->addPositiveSample(img, _bboxes);
    return ARTOS_RES_OK;
}

bool progress_proxy(unsigned int current, unsigned int total, void * data)
{
    progress_cb_t cb = reinterpret_cast<progress_cb_t>(data);
    return cb(current, total);
}



//------------------------------------------------------------------
//---------------------- Background Statistics ---------------------
//------------------------------------------------------------------

int learn_bg(const char * repo_directory, const char * bg_file,
             const unsigned int num_images, const unsigned int max_offset, overall_progress_cb_t progress_cb,
             const bool accurate_autocorrelation)
{
    // Check repository
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    MixedImageIterator imgIt(repo_directory, 1);
    
    // Setup some stuff for progress callback
    progress_params progParams;
    progParams.cb = progress_cb;
    progParams.overall_step = 0;
    progParams.overall_steps_total = 2;
    progParams.aborted = false;
    
    // Learn background statistics
    StationaryBackground bg;
    try {
        bg.learnMean(imgIt, num_images, (progress_cb != NULL) ? &populate_progress : NULL, reinterpret_cast<void*>(&progParams));
    } catch (const UseBeforeSetupException & e) {
        return ARTOS_LEARN_RES_FEATURE_EXTRACTOR_NOT_READY;
    }
    if (progParams.aborted)
        return ARTOS_RES_ABORTED;
    progParams.overall_step++;
    if (accurate_autocorrelation)
        bg.learnCovariance_accurate(imgIt, num_images, max_offset, (progress_cb != NULL) ? &populate_progress : NULL, reinterpret_cast<void*>(&progParams));
    else
        bg.learnCovariance(imgIt, num_images, max_offset, (progress_cb != NULL) ? &populate_progress : NULL, reinterpret_cast<void*>(&progParams));
    if (progParams.aborted)
        return ARTOS_RES_ABORTED;
    if (progress_cb != NULL)
        progress_cb(progParams.overall_steps_total, progParams.overall_steps_total, 0, 0);
    return (bg.writeToFile(bg_file)) ? ARTOS_RES_OK : ARTOS_RES_FILE_ACCESS_DENIED;
}



//--------------------------------------------------------------------
//---------------------------- Evaluation ----------------------------
//--------------------------------------------------------------------

int evaluator_add_samples_from_synset(const unsigned int detector, const char * repo_directory, const char * synset_id,
                                      const unsigned int num_negative)
{
    // Check detector handle
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    // Check image repository
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    ImageRepository repo(repo_directory);
    
    // Search synset
    Synset synset = repo.getSynset(synset_id);
    if (synset.id.empty())
        return ARTOS_IMGREPO_RES_SYNSET_NOT_FOUND;

    // Extract positive samples
    vector<Sample*> & positives = eval_positive_samples[detector];
    for (SynsetImageIterator imgIt = synset.getImageIterator(false); imgIt.ready(); ++imgIt)
    {
        SynsetImage simg = *imgIt;
        JPEGImage & img = simg.getImage();
        if (!img.empty())
        {
            Sample * s = new Sample();
            s->m_simg = simg;
            if (simg.loadBoundingBoxes())
                s->m_bboxes = simg.bboxes;
            else
                s->m_bboxes.assign(1, ARTOS::Rectangle(0, 0, img.width(), img.height()));
            s->modelAssoc.assign(s->bboxes().size(), Sample::noAssoc);
            s->data = NULL;
            positives.push_back(s);
        }
    }
    
    // Extract negative samples
    if (num_negative > 0)
    {
        vector<JPEGImage> & negatives = eval_negative_samples[detector];
        for (SynsetIterator synsetIt = repo.getSynsetIterator(); synsetIt.ready(); ++synsetIt)
        {
            Synset negSynset = *synsetIt;
            if (negSynset.id != synset.id)
                for (SynsetImageIterator imgIt = negSynset.getImageIterator(); imgIt.ready(); ++imgIt)
                {
                    SynsetImage simg = *imgIt;
                    JPEGImage & img = simg.getImage();
                    if (!img.empty())
                        negatives.push_back(img);
                }
        }
    }
    
    return ARTOS_RES_OK;
}

int evaluator_add_positive_file(const unsigned int detector, const char * imagefile, const char * annotation_file)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    // Load image
    JPEGImage img(imagefile);
    if (img.empty())
        return ARTOS_DETECT_RES_INVALID_IMG_DATA;
    
    // Load annotations
    Scene scene(annotation_file);
    if (scene.empty())
        return ARTOS_DETECT_RES_INVALID_ANNOTATIONS;
    double scale = static_cast<double>(scene.width()) / img.width();
    
    // Set up sample
    Sample * s = new Sample();
    for (vector<Object>::const_iterator objIt = scene.objects().begin(); objIt != scene.objects().end(); objIt++)
    {
        Rectangle bbox = objIt->bndbox();
        bbox.setX(round(bbox.x() * scale));
        bbox.setY(round(bbox.y() * scale));
        bbox.setWidth(round(bbox.width() * scale));
        bbox.setHeight(round(bbox.height() * scale));
        if (bbox.x() > 0 && bbox.y() > 0 && bbox.x() < img.width() && bbox.y() < img.height() && bbox.width() > 0 && bbox.height() > 0)
            s->m_bboxes.push_back(bbox);
    }
    s->m_img = move(img);
    s->modelAssoc.assign(s->bboxes().size(), Sample::noAssoc);
    s->data = NULL;
    eval_positive_samples[detector].push_back(s);
    
    return ARTOS_RES_OK;
}

int evaluator_add_positive_jpeg(const unsigned int detector, const JPEGImage & img, const FlatBoundingBox * bboxes, const unsigned int num_bboxes)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    if (img.empty())
        return ARTOS_DETECT_RES_INVALID_IMG_DATA;
    
    Sample * s = new Sample();
    s->m_img = img;
    if (bboxes != NULL && num_bboxes > 0)
    {
        for (const FlatBoundingBox * flat_bbox = bboxes; flat_bbox < bboxes + num_bboxes; flat_bbox++)
            s->m_bboxes.push_back(Rectangle(flat_bbox->left, flat_bbox->top, flat_bbox->width, flat_bbox->height));
    }
    else
        s->m_bboxes.assign(1, ARTOS::Rectangle(0, 0, img.width(), img.height()));
    s->modelAssoc.assign(s->bboxes().size(), Sample::noAssoc);
    s->data = NULL;
    eval_positive_samples[detector].push_back(s);
    
    return ARTOS_RES_OK;
}

int evaluator_add_positive_file_jpeg(const unsigned int detector, const char * imagefile, const FlatBoundingBox * bboxes, const unsigned int num_bboxes)
{
    return evaluator_add_positive_jpeg(detector, JPEGImage(imagefile), bboxes, num_bboxes);
}

int evaluator_add_positive_raw(const unsigned int detector,
                               const unsigned char * img_data, const unsigned int img_width, const unsigned int img_height, const bool grayscale,
                               const FlatBoundingBox * bboxes, const unsigned int num_bboxes)
{
    return evaluator_add_positive_jpeg(detector, JPEGImage(img_width, img_height, (grayscale) ? 1 : 3, img_data), bboxes, num_bboxes);
}

int evaluator_add_negative_file_jpeg(const unsigned int detector, const char * imagefile)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    JPEGImage img(imagefile);
    if (img.empty())
        return ARTOS_DETECT_RES_INVALID_IMG_DATA;
    
    eval_negative_samples[detector].push_back(move(img));
    return ARTOS_RES_OK;
}

int evaluator_add_negative_raw(const unsigned int detector,
                               const unsigned char * img_data, const unsigned int img_width, const unsigned int img_height, const bool grayscale)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    JPEGImage img(img_width, img_height, (grayscale) ? 1 : 3, img_data);
    if (img.empty())
        return ARTOS_DETECT_RES_INVALID_IMG_DATA;
    
    eval_negative_samples[detector].push_back(move(img));
    return ARTOS_RES_OK;
}

int evaluator_run(const unsigned int detector, const unsigned int granularity, const double eq_overlap, progress_cb_t progress_cb)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    ModelEvaluator * det = detectors[detector - 1];
    if (det->getNumModels() == 0)
        return ARTOS_DETECT_RES_NO_MODELS;
    if (eval_positive_samples[detector].empty())
        return ARTOS_DETECT_RES_NO_IMAGES;
    
    ProgressCallback progressCB = (progress_cb != NULL) ? &progress_proxy : NULL;
    void * cbData = (progress_cb != NULL) ? reinterpret_cast<void*>(progress_cb) : NULL;
    det->setEqOverlap(eq_overlap);
    det->testModels(
        eval_positive_samples[detector], 0,
        (!eval_negative_samples[detector].empty()) ? &eval_negative_samples[detector] : NULL,
        granularity, progressCB, cbData
    );
    return ARTOS_RES_OK;
}

int evaluator_get_raw_results(const unsigned int detector, RawTestResult * result_buf, unsigned int * result_buf_size, const unsigned int model_index)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    ModelEvaluator * det = detectors[detector - 1];
    if (model_index >= det->getNumModels())
        return ARTOS_RES_INDEX_OUT_OF_BOUNDS;
    
    const vector<ModelEvaluator::TestResult> & results = det->getResults(model_index);
    if (result_buf)
    {
        RawTestResult * result = result_buf;
        unsigned int i;
        for (i = 0; i < *result_buf_size && i < results.size(); i++, result++)
        {
            result->threshold = results[i].threshold;
            result->tp = results[i].tp;
            result->fp = results[i].fp;
            result->np = results[i].np;
        }
        *result_buf_size = i;
    }
    else
        *result_buf_size = results.size();
    
    return (results.empty()) ? ARTOS_DETECT_RES_NO_RESULTS : ARTOS_RES_OK;
}

int evaluator_get_max_fmeasure(const unsigned int detector, float * fmeasure, float * threshold, const unsigned int model_index)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    ModelEvaluator * det = detectors[detector - 1];
    if (model_index >= det->getNumModels())
        return ARTOS_RES_INDEX_OUT_OF_BOUNDS;
    if (det->getResults(model_index).empty())
        return ARTOS_DETECT_RES_NO_RESULTS;
    
    pair<float, float> fm = det->getMaxFMeasure(model_index);
    if (fmeasure)
        *fmeasure = fm.second;
    if (threshold)
        *threshold = fm.first;
    return ARTOS_RES_OK;
}

int evaluator_get_fmeasure_at(const unsigned int detector, const float threshold, float * fmeasure, const unsigned int model_index)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    ModelEvaluator * det = detectors[detector - 1];
    if (model_index >= det->getNumModels())
        return ARTOS_RES_INDEX_OUT_OF_BOUNDS;
    if (det->getResults(model_index).empty())
        return ARTOS_DETECT_RES_NO_RESULTS;
    
    if (fmeasure)
        *fmeasure = det->getFMeasureAt(threshold, model_index);
    return ARTOS_RES_OK;
}

int evaluator_get_ap(const unsigned int detector, float * ap, const unsigned int model_index)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    ModelEvaluator * det = detectors[detector - 1];
    if (model_index >= det->getNumModels())
        return ARTOS_RES_INDEX_OUT_OF_BOUNDS;
    if (det->getResults(model_index).empty())
        return ARTOS_DETECT_RES_NO_RESULTS;
    
    if (ap)
        *ap = det->computeAveragePrecision(model_index);
    return ARTOS_RES_OK;
}

int evaluator_dump_results(const unsigned int detector, const char * dump_file)
{
    if (!is_valid_detector_handle(detector))
        return ARTOS_RES_INVALID_HANDLE;
    
    ModelEvaluator * det = detectors[detector - 1];
    if (det->getResults().empty())
        return ARTOS_DETECT_RES_NO_RESULTS;
    
    return (det->dumpTestResults(dump_file, -1, true, ModelEvaluator::PRECISION | ModelEvaluator::RECALL | ModelEvaluator::FMEASURE))
           ? ARTOS_RES_OK : ARTOS_RES_FILE_ACCESS_DENIED;
}



//------------------------------------------------------------------
//---------------------------- Settings ----------------------------
//------------------------------------------------------------------


int change_feature_extractor(const char * type)
{
    try {
        FeatureExtractor::setDefaultFeatureExtractor(type);
        return ARTOS_RES_OK;
    } catch (const UnknownFeatureExtractorException & e) {
        return ARTOS_SETTINGS_RES_UNKNOWN_FEATURE_EXTRACTOR;
    }
}


int feature_extractor_get_info(FeatureExtractorInfo * info)
{
    if (info)
    {
        shared_ptr<FeatureExtractor> fe = FeatureExtractor::defaultFeatureExtractor();
        memset(info->type, 0, sizeof(info->type));
        strncpy(info->type, fe->type(), sizeof(info->type) - 1);
        memset(info->name, 0, sizeof(info->name));
        strncpy(info->name, fe->name(), sizeof(info->name) - 1);
    }
    return ARTOS_RES_OK;
}


int list_feature_extractors(FeatureExtractorInfo * info_buf, unsigned int * info_buf_size)
{
    if (info_buf)
    {
        vector< shared_ptr<FeatureExtractor> > featureExtractors;
        FeatureExtractor::listFeatureExtractors(featureExtractors);
        FeatureExtractorInfo * info = info_buf;
        unsigned int i;
        for (i = 0; i < *info_buf_size && i < featureExtractors.size(); i++, info++)
        {
            memset(info->type, 0, sizeof(info->type));
            strncpy(info->type, featureExtractors[i]->type(), sizeof(info->type) - 1);
            memset(info->name, 0, sizeof(info->name));
            strncpy(info->name, featureExtractors[i]->name(), sizeof(info->name) - 1);
        }
        *info_buf_size = i;
    }
    else
        *info_buf_size = static_cast<unsigned int>(FeatureExtractor::numFeatureExtractors());
    return ARTOS_RES_OK;
}


static int write_fe_params_to_buffer(const vector<FeatureExtractor::ParameterInfo> & params, FeatureExtractorParameter * param_buf, unsigned int * param_buf_size)
{
    if (param_buf)
    {
        FeatureExtractorParameter * info = param_buf;
        unsigned int i;
        for (i = 0; i < *param_buf_size && i < params.size(); i++, info++)
        {
            memset(info->name, 0, sizeof(info->name));
            params[i].name.copy(info->name, sizeof(info->name) - 1);
            switch (params[i].type)
            {
                case FeatureExtractor::ParameterType::INT:
                    info->type = ARTOS_PARAM_TYPE_INT;
                    info->val.intVal = static_cast<int>(params[i].intValue);
                    break;
                case FeatureExtractor::ParameterType::SCALAR:
                    info->type = ARTOS_PARAM_TYPE_SCALAR;
                    info->val.scalarVal = static_cast<float>(params[i].scalarValue);
                    break;
                case FeatureExtractor::ParameterType::STRING:
                    info->type = ARTOS_PARAM_TYPE_STRING;
                    info->val.stringVal = params[i].stringValue;
                    break;
                default:
                    info->type = static_cast<unsigned int>(params[i].type);
            }
        }
        *param_buf_size = i;
    }
    else
        *param_buf_size = params.size();
    return ARTOS_RES_OK;
}


int list_feature_extractor_params(const char * type, FeatureExtractorParameter * param_buf, unsigned int * param_buf_size)
{
    shared_ptr<FeatureExtractor> fe;
    try {
        fe = FeatureExtractor::create(type);
    } catch (const UnknownFeatureExtractorException & e) {
        return ARTOS_SETTINGS_RES_UNKNOWN_FEATURE_EXTRACTOR;
    }
    
    vector<FeatureExtractor::ParameterInfo> params;
    fe->listParameters(params);
    return write_fe_params_to_buffer(params, param_buf, param_buf_size);
}


int feature_extractor_list_params(FeatureExtractorParameter * param_buf, unsigned int * param_buf_size)
{
    vector<FeatureExtractor::ParameterInfo> params;
    FeatureExtractor::defaultFeatureExtractor()->listParameters(params);
    return write_fe_params_to_buffer(params, param_buf, param_buf_size);
}


int feature_extractor_set_int_param(const char * param_name, int value)
{
    try {
        FeatureExtractor::defaultFeatureExtractor()->setParam(param_name, static_cast<int32_t>(value));
    } catch (const UnknownParameterException & e) {
        return ARTOS_SETTINGS_RES_UNKNOWN_PARAMETER;
    } catch (const invalid_argument & e) {
        return ARTOS_SETTINGS_RES_INVALID_PARAMETER_VALUE;
    }
    return ARTOS_RES_OK;
}


int feature_extractor_set_scalar_param(const char * param_name, float value)
{
    try {
        FeatureExtractor::defaultFeatureExtractor()->setParam(param_name, static_cast<FeatureScalar>(value));
    } catch (const UnknownParameterException & e) {
        return ARTOS_SETTINGS_RES_UNKNOWN_PARAMETER;
    } catch (const invalid_argument & e) {
        return ARTOS_SETTINGS_RES_INVALID_PARAMETER_VALUE;
    }
    return ARTOS_RES_OK;
}


int feature_extractor_set_string_param(const char * param_name, const char * value)
{
    try {
        FeatureExtractor::defaultFeatureExtractor()->setParam(param_name, value);
    } catch (const UnknownParameterException & e) {
        return ARTOS_SETTINGS_RES_UNKNOWN_PARAMETER;
    } catch (const invalid_argument & e) {
        return ARTOS_SETTINGS_RES_INVALID_PARAMETER_VALUE;
    }
    return ARTOS_RES_OK;
}



//------------------------------------------------------------------
//---------------------------- ImageNet ----------------------------
//------------------------------------------------------------------


bool check_repository_directory(const char * repo_directory, const char ** err_msg)
{
    return ImageRepository::hasRepositoryStructure(repo_directory, err_msg);
}

const char * get_image_repository_type()
{
    return ImageRepository::type();
}

int list_synsets(const char * repo_directory, SynsetSearchResult * synset_buf, unsigned int * synset_buf_size)
{
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    ImageRepository repo(repo_directory);
    if (synset_buf == NULL || *synset_buf_size == 0)
        *synset_buf_size = repo.getNumSynsets();
    else
    {
        vector<string> ids, descriptions;
        repo.listSynsets(&ids, &descriptions);
        size_t i;
        for (i = 0; i < ids.size() && i < *synset_buf_size; i++, synset_buf++)
        {
            memset(synset_buf->synsetId, 0, sizeof(synset_buf->synsetId));
            ids[i].copy(synset_buf->synsetId, sizeof(synset_buf->synsetId) - 1);
            memset(synset_buf->description, 0, sizeof(synset_buf->description));
            descriptions[i].copy(synset_buf->description, sizeof(synset_buf->description) - 1);
            synset_buf->score = 0;
        }
        *synset_buf_size = i;
    }
    return ARTOS_RES_OK;
}

int search_synsets(const char * repo_directory, const char * phrase, SynsetSearchResult * result_buf, unsigned int * result_buf_size)
{
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    ImageRepository repo(repo_directory);
    vector<Synset> results;
    vector<float> scores;
    repo.searchSynsets(phrase, results, *result_buf_size, &scores);
    size_t i;
    for (i = 0; i < results.size() && i < *result_buf_size; i++, result_buf++)
    {
        memset(result_buf->synsetId, 0, sizeof(result_buf->synsetId));
        results[i].id.copy(result_buf->synsetId, sizeof(result_buf->synsetId) - 1);
        memset(result_buf->description, 0, sizeof(result_buf->description));
        results[i].description.copy(result_buf->description, sizeof(result_buf->description) - 1);
        result_buf->score = scores[i];
    }
    *result_buf_size = i;
    return ARTOS_RES_OK;
}

int extract_images_from_synset(const char * repo_directory, const char * synset_id, const char * out_directory, unsigned int * num_images)
{
    // Check repository
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    // Check output directory
    if (!is_dir(out_directory))
        return ARTOS_RES_DIRECTORY_NOT_FOUND;
    // Check pointer parameters
    if (num_images == NULL)
        return ARTOS_RES_OK; // extract nothing
    
    // Search synset
    Synset synset = ImageRepository(repo_directory).getSynset(synset_id);
    if (synset.id.empty())
        return ARTOS_IMGREPO_RES_SYNSET_NOT_FOUND;
        
    // Extract
    SynsetImageIterator imgIt = synset.getImageIterator();
    for (; imgIt.ready() && (unsigned int) imgIt < *num_images; ++imgIt)
    {
        SynsetImage simg = *imgIt;
        JPEGImage img = simg.getImage();
        if (!img.empty())
            img.save(join_path(2, out_directory, (simg.getFilename() + ".jpg").c_str()));
    }
    *num_images = imgIt.pos();
    return ARTOS_RES_OK;
}

int extract_samples_from_synset(const char * repo_directory, const char * synset_id, const char * out_directory, unsigned int * num_samples)
{
    // Check repository
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    // Check output directory
    if (!is_dir(out_directory))
        return ARTOS_RES_DIRECTORY_NOT_FOUND;
    // Check pointer parameters
    if (num_samples == NULL)
        return ARTOS_RES_OK; // extract nothing
    
    // Search synset
    Synset synset = ImageRepository(repo_directory).getSynset(synset_id);
    if (synset.id.empty())
        return ARTOS_IMGREPO_RES_SYNSET_NOT_FOUND;
        
    // Extract
    SynsetImageIterator imgIt = synset.getImageIterator(true);
    unsigned int count = 0;
    char extBuf[10];
    vector<JPEGImage> samples;
    for (; imgIt.ready() && count < *num_samples; ++imgIt)
    {
        SynsetImage simg = *imgIt;
        samples.clear();
        simg.getSamplesFromBoundingBoxes(samples);
        for (size_t i = 0; i < samples.size() && count < *num_samples; i++)
        {
            sprintf(extBuf, "_%lu.jpg", i + 1);
            samples[i].save(join_path(2, out_directory, (simg.getFilename() + extBuf).c_str()));
            count++;
        }
    }
    *num_samples = count;
    return ARTOS_RES_OK;
}

int extract_mixed_images(const char * repo_directory, const char * out_directory, const unsigned int num_images, const unsigned int per_synset)
{
    // Check repository
    if (!ImageRepository::hasRepositoryStructure(repo_directory))
        return ARTOS_IMGREPO_RES_INVALID_REPOSITORY;
    // Check output directory
    string out_dir(out_directory);
    if (!is_dir(out_dir))
        return ARTOS_RES_DIRECTORY_NOT_FOUND;
    
    // Extract
    MixedImageIterator imgIt = ImageRepository(repo_directory).getMixedIterator(per_synset);
    for (; imgIt.ready() && (unsigned int) imgIt < num_images; ++imgIt)
        imgIt.extract(out_dir);
    return ARTOS_RES_OK;
}
