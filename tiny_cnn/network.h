/*
    Copyright (c) 2013, Taiga Nomi
    All rights reserved.
    
    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY 
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY 
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND 
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS 
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <iterator>
#include <iomanip>
#include <map>
#include <set>

#include "tiny_cnn/util/util.h"
#include "tiny_cnn/layers/layers.h"
#include "tiny_cnn/lossfunctions/loss_function.h"
#include "tiny_cnn/activations/activation_function.h"

namespace tiny_cnn {

struct result {
    result() : num_success(0), num_total(0) {}

    double accuracy() const {
        return num_success * 100.0 / num_total;
    }

    template <typename Char, typename CharTraits>
    void print_summary(std::basic_ostream<Char, CharTraits>& os) const {
        os << "accuracy:" << accuracy() << "% (" << num_success << "/" << num_total << ")" << std::endl;
    }

    template <typename Char, typename CharTraits>
    void print_detail(std::basic_ostream<Char, CharTraits>& os) {
        print_summary(os);
        auto all_labels = labels();

        os << std::setw(5) << "*" << " ";
        for (auto c : all_labels) 
            os << std::setw(5) << c << " ";
        os << std::endl;

        for (auto r : all_labels) {
            os << std::setw(5) << r << " ";           
            for (auto c : all_labels) 
                os << std::setw(5) << confusion_matrix[r][c] << " ";
            os << std::endl;
        }
    }

    std::set<label_t> labels() const {
        std::set<label_t> all_labels;
        for (auto r : confusion_matrix) {
            all_labels.insert(r.first);
            for (auto c : r.second)
                all_labels.insert(c.first);
        }
        return all_labels;
    }

    int num_success;
    int num_total;
    std::map<label_t, std::map<label_t, int> > confusion_matrix;
};

enum grad_check_mode {
    GRAD_CHECK_ALL, ///< check all elements of weights
    GRAD_CHECK_RANDOM ///< check 10 randomly selected weights
};

template<typename LossFunction, typename Optimizer>
class network 
{
public:
    typedef LossFunction E;

    explicit network(const std::string& name = "") : name_(name) {}

    /**
     * return input dims of network
     **/
    layer_size_t in_dim() const         { return layers_.head()->in_size(); }

    /**
     * return output dims of network
     **/
    layer_size_t out_dim() const        { return layers_.tail()->out_size(); }

    std::string  name() const           { return name_; }
    Optimizer&   optimizer()            { return optimizer_; }

    /**
     * explicitly initialize weights of all layers
     **/
    void         init_weight()          { layers_.init_weight(); }

    /**
     * add one layer to tail(output-side)
     **/
    void         add(std::shared_ptr<layer_base> layer) { layers_.add(layer); }

    /**
     * executes forward-propagation and returns output
     **/
    vec_t        predict(const vec_t& in) { return fprop(in); }

    /**
     * executes forward-propagation and returns maximum output
     **/
    float_t      predict_max_value(const vec_t& in) {
        return fprop_max(in);
    }
    /**
     * executes forward-propagation and returns maximum output index
     **/
    label_t      predict_label(const vec_t& in) {
        return fprop_max_index(in);
    }

    /**
     * executes forward-propagation and returns output
     *
     * @param in input value range(double[], std::vector<double>, std::list<double> etc)
     **/
    template <typename Range>
    vec_t        predict(const Range& in) {
        using std::begin; // for ADL
        using std::end;
        return predict(vec_t(begin(in), end(in)));
    }

    /**
     * training conv-net
     *
     * @param in_size              number of training vectors
     * @param input_data_function  function enumerating input data, taking a int in the range 0-(in_size-1)
     *                             and returning a vec_t or const vec_t&
     * @param output_data_function function enumerating expected results, taking a int in the range 0-(in_size-1),
     *                             a second int (task id - can be ignored in most situations),
     *                             and returning either a vec_t/const vec_t& (expected vector), 
     *                             a label_t (expected label) or a pair<label_t, float_t> 
     *                             (this last overload can be used to update a single value in the resulting vector)
     * @param epoch                number of training epochs
     * @param on_batch_enumerate   callback for each mini-batch enumerate
     * @param on_epoch_enumerate   callback for each epoch 
     * @param reset_weights        reset all weights or keep current
     * @param n_threads            number of tasks
     */
    template <typename InputDataFunction, typename OutputDataFunction, typename OnBatchEnumerate, typename OnEpochEnumerate>
    bool train(size_t in_size,
               const InputDataFunction  &input_data_function,
               const OutputDataFunction &output_data_function,
               size_t                    batch_size,
               int                       epoch,
               const OnBatchEnumerate   &on_batch_enumerate,
               const OnEpochEnumerate   &on_epoch_enumerate,

               const bool                reset_weights = true,
               const int                 n_threads = CNN_TASK_SIZE
               )
    {
        if (reset_weights)
            init_weight();
        layers_.set_parallelize(batch_size < CNN_TASK_SIZE);
        optimizer_.reset();

        for (int iter = 0; iter < epoch; iter++) {
            if (optimizer_.requires_hessian())
                calc_hessian(in_size, input_data_function);
            for (size_t i = 0; i < in_size; i += batch_size) {
                train_once(i, std::min(batch_size, in_size - i), input_data_function, output_data_function, n_threads);
                on_batch_enumerate();

                if (i % 100 == 0 && layers_.is_exploded()) {
                    std::cout << "[Warning]Detected infinite value in weight. stop learning." << std::endl;
                    return false;
                }
            }
            on_epoch_enumerate();
        }
        return true;
    }

    /**
     * training conv-net
     *
     * @param in                 array of input data
     * @param t                  array of training signals(label, vector or update pair (see the other overload)
     * @param epoch              number of training epochs
     * @param on_batch_enumerate callback for each mini-batch enumerate
     * @param on_epoch_enumerate callback for each epoch
     * @param reset_weights      reset all weights or keep current
     * @param n_threads          number of tasks
     */
    template <typename OnBatchEnumerate, typename OnEpochEnumerate, typename T>
    bool train(const std::vector<vec_t>& in,
               const std::vector<T>&     t,
               size_t                    batch_size,
               int                       epoch,
               const OnBatchEnumerate    &on_batch_enumerate,
               const OnEpochEnumerate    &on_epoch_enumerate,

               const bool                reset_weights = true,
               const int                 n_threads = CNN_TASK_SIZE
               )
    {
        check_training_data(in, t);
        return train(in.size(), [&in](int i) {return in[i]; }, [&t](int i) {return t[i]; }, batch_size, epoch, on_batch_enumerate, on_epoch_enumerate, reset_weights, n_threads);
    }

    /**
     * training conv-net without callback
     **/
    template<typename T>
    bool train(const std::vector<vec_t>& in, const std::vector<T>& t, size_t batch_size = 1, int epoch = 1) {
        return train(in, t, batch_size, epoch, nop, nop);
    }

    /**
     * test and generate confusion-matrix for classification task
     **/
    result test(const std::vector<vec_t>& in, const std::vector<label_t>& t) {
        result test_result;

        for (size_t i = 0; i < in.size(); i++) {
            const label_t predicted = fprop_max_index(in[i]);
            const label_t actual = t[i];

            if (predicted == actual) test_result.num_success++;
            test_result.num_total++;
            test_result.confusion_matrix[predicted][actual]++;
        }
        return test_result;
    }

    std::vector<vec_t> test(const std::vector<vec_t>& in)
     {
            std::vector<vec_t> test_result(in.size());

            for_i(in.size(), [&](int i)
            {
                test_result[i] = predict(in[i]);
            });
            return test_result;
    }

    /**
     * calculate loss value (the smaller, the better) for regression task
     **/
    float_t get_loss(const std::vector<vec_t>& in, const std::vector<vec_t>& t) {
        float_t sum_loss = float_t(0);

        for (size_t i = 0; i < in.size(); i++) {
            const vec_t predicted = predict(in[i]);
            sum_loss += get_loss(predict(in[i]), t[i]);
        }
        return sum_loss;
    }

    /**
     * save network weights into stream
     * @attention this saves only network *weights*, not network configuration
     **/
    void save(std::ostream& os) const {
        os.precision(std::numeric_limits<tiny_cnn::float_t>::digits10);

        auto l = layers_.head();
        while (l) { l->save(os); l = l->next(); }
    }

    /**
     * load network weights from stream
     * @attention this loads only network *weights*, not network configuration
     **/
    void load(std::istream& is) {
        is.precision(std::numeric_limits<tiny_cnn::float_t>::digits10);

        auto l = layers_.head();
        while (l) { l->load(is); l = l->next(); }
    }

    /**
     * checking gradients calculated by bprop
     * detail information:
     * http://ufldl.stanford.edu/wiki/index.php/Gradient_checking_and_advanced_optimization
     **/
    bool gradient_check(const vec_t* in, const label_t* t, int data_size, float_t eps, grad_check_mode mode) {
        assert(!layers_.empty());
        std::vector<vec_t> v;
        label2vector(t, data_size, &v);

        auto current = layers_.head();

        while ((current = current->next()) != 0) { // ignore first input layer
            vec_t& w = current->weight();
            vec_t& b = current->bias();
            vec_t& dw = current->weight_diff(0);
            vec_t& db = current->bias_diff(0);

            if (w.empty()) continue;
            
            switch (mode) {
            case GRAD_CHECK_ALL:
                for (int i = 0; i < (int)w.size(); i++)
                    if (calc_delta(in, &v[0], data_size, w, dw, i) > eps) return false;
                for (int i = 0; i < (int)b.size(); i++)
                    if (calc_delta(in, &v[0], data_size, b, db, i) > eps) return false;
                break;
            case GRAD_CHECK_RANDOM:
                for (int i = 0; i < 10; i++)
                    if (calc_delta(in, &v[0], data_size, w, dw, uniform_idx(w)) > eps) return false;
                for (int i = 0; i < 10; i++)
                    if (calc_delta(in, &v[0], data_size, b, db, uniform_idx(b)) > eps) return false;
                break;
            default:
                throw nn_error("unknown grad-check type");
            }
        }
        return true;
    }

    template <typename L, typename O>
    bool has_same_weights(const network<L, O>& others, float_t eps) const {
        auto h1 = layers_.head();
        auto h2 = others.layers_.head();

        while (h1 && h2) {
            if (!h1->has_same_weights(*h2, eps))
                return false;
            h1 = h1->next();
            h2 = h2->next();
        }
        return true;
    }

    /**
     * return index-th layer as <T>
     * throw nn_error if index-th layer cannot be converted to T
     **/
    template <typename T>
    const T& at(size_t index) const {
        return layers_.at<T>(index);
    }

    /**
     * return raw pointer of index-th layer
     **/
    const layer_base* operator [] (size_t index) const {
        return layers_[index];
    }

    /**
     * return raw pointer of index-th layer
     **/
    layer_base* operator [] (size_t index) {
        return layers_[index];
    }

    /**
     * number of layers
     **/
    size_t depth() const {
        return layers_.depth();
    }

    /**
     * input shape (width x height x channels)
     **/
    index3d<layer_size_t> in_shape() const {
        return layers_.head()->in_shape();
    }

    /**
     * set weight initializer to all layers
     **/
    template <typename WeightInit>
    network& weight_init(const WeightInit& f) {
        auto ptr = std::make_shared<WeightInit>(f);
        for (size_t i = 0; i < depth(); i++)
          layers_[i]->weight_init(ptr);
        return *this;
    }

    /**
     * set bias initializer to all layers
     **/
    template <typename BiasInit>
    network& bias_init(const BiasInit& f) { 
        auto ptr = std::make_shared<BiasInit>(f);
        for (size_t i = 0; i < depth(); i++)
            layers_[i]->bias_init(ptr);
        return *this;
    }

protected:
    float_t fprop_max(const vec_t& in, int idx = 0) {
        const vec_t& prediction = fprop(in, idx);
        return *std::max_element(std::begin(prediction), std::end(prediction));
    }

    label_t fprop_max_index(const vec_t& in, int idx = 0) {
        return label_t(max_index(fprop(in, idx)));
    }
private:

    vec_t label2vector(label_t t) const {
        vec_t result(out_dim(), target_value_min());
        result[t] = target_value_max();
        return result;
    }

    template <typename InputDataFunction, typename OutputDataFunction>
    void train_once(size_t in_offset, size_t in_size, const InputDataFunction &input_data_function, const OutputDataFunction &output_data_function, const int num_tasks = CNN_TASK_SIZE) {
        if (in_size == 1) { // TODO: check if we really need this optimization, or the underlying threading frameworks will evaluate a single for_i in the main thread.
            const auto &t = output_data_function(in_offset, 0); // Note: do not inline, this might use fprop on index i internally. No extra copy is performed: if output_data_functions returns a value rather than a reference, "temporary life extension" will keep it alive until end-of-scope.
            bprop(fprop(input_data_function(0)), t);
            layers_.update_weights(&optimizer_, 1, 1);
        } else {
            train_onebatch(in_offset, in_size, input_data_function, output_data_function, num_tasks);
        }
    }

    template <typename InputDataFunction, typename OutputDataFunction>
    void train_onebatch(size_t in_offset, size_t in_size, const InputDataFunction &input_data_function, const OutputDataFunction &output_data_function, const int num_tasks = CNN_TASK_SIZE) {
        int num_threads = std::min(int(in_size), num_tasks);
        int data_per_thread = (in_size + num_threads - 1) / num_threads;
        int last_index = in_offset + in_size;
        for_i(num_threads, [&](int i) {
            int start_index = in_offset + i * data_per_thread;
            int end_index = std::min(last_index, start_index + data_per_thread);

            for (int j = start_index; j < end_index; ++j) {
                const auto &t = output_data_function(j, i); // Note: do not inline, this might use fprop on index i internally. No extra copy is performed: if output_data_functions returns a value rather than a reference, "temporary life extension" will keep it alive until end-of-scope.
                bprop(fprop(input_data_function(j), i), t, i);
            }
        }, 1);

        // merge all dW and update W by optimizer
        layers_.update_weights(&optimizer_, num_threads, in_size);
    }

    template <typename InputDataFunction>
    void calc_hessian(size_t in_size, InputDataFunction &input_data_function, size_t size_initialize_hessian = 500) {
        int size = std::min(in_size, size_initialize_hessian);

        for (int i = 0; i < size; i++)
            bprop_2nd(fprop(input_data_function(i)));

        layers_.divide_hessian(size);
    }

    template<typename Activation>
    bool is_canonical_link(const Activation& h) {
        if (typeid(h) == typeid(activation::sigmoid) && typeid(E) == typeid(cross_entropy)) return true;
        if (typeid(h) == typeid(activation::tan_h) && typeid(E) == typeid(cross_entropy)) return true;
        if (typeid(h) == typeid(activation::identity) && typeid(E) == typeid(mse)) return true;
        if (typeid(h) == typeid(activation::softmax) && typeid(E) == typeid(cross_entropy_multiclass)) return true;
        return false;
    }

    const vec_t& fprop(const vec_t& in, int idx = 0) {
        if (in.size() != size_t(in_dim()))
            data_mismatch(*layers_[0], in);
        return layers_.head()->forward_propagation(in, idx);
    }

    float_t get_loss(const vec_t& out, const vec_t& t) {
        float_t e = float_t(0);
        assert(out.size() == t.size());
        for_i(out.size(), [&](int i){ e += E::f(out[i], t[i]); });
        return e;
    }

    void bprop_2nd(const vec_t& out) {
        vec_t delta(out_dim());
        const activation::function& h = layers_.tail()->activation_function();

        if (is_canonical_link(h)) {
            for_i(out_dim(), [&](int i){ delta[i] = target_value_max() * h.df(out[i]);});
        } else {
            for_i(out_dim(), [&](int i){ delta[i] = target_value_max() * h.df(out[i]) * h.df(out[i]);}); // FIXME
        }

        layers_.tail()->back_propagation_2nd(delta);
    }

    void bprop(const vec_t& out, const vec_t& t, int idx = 0) {
        vec_t delta(out_dim());
        const activation::function& h = layers_.tail()->activation_function();

        if (is_canonical_link(h)) {
            for_i(out_dim(), [&](int i){ delta[i] = out[i] - t[i]; });
        } else {
            vec_t dE_dy = gradient<E>(out, t);

            // delta = dE/da = (dE/dy) * (dy/da)
            for (size_t i = 0; i < out_dim(); i++) {
                vec_t dy_da = h.df(out, i);
                delta[i] = vectorize::dot(&dE_dy[0], &dy_da[0], out_dim());
            }
        }

        layers_.tail()->back_propagation(delta, idx);
    }

    void bprop(const vec_t& out, label_t t, int idx = 0) {
        bprop(out, label2vector(t), idx);
    }

    void bprop(const vec_t& out, const std::pair<label_t, float_t>& t, int idx = 0) {
        vec_t delta;
        const activation::function& h = layers_.tail()->activation_function();

        if (is_canonical_link(h)) {
            delta.resize(out_dim(), float_t(0));
            delta[t.first] = out[t.first] - t.second;
        } else {
            vec_t t_vector = out;
            t_vector[t.first] = t.second;
            vec_t dE_dy = gradient<E>(out, t_vector);
            delta.reserve(out_dim());
            // delta = dE/da = (dE/dy) * (dy/da)
            for (size_t i = 0; i < out_dim(); i++) {
                vec_t dy_da = h.df(out, i);
                delta.push_back(vectorize::dot(&dE_dy[0], &dy_da[0], out_dim()));
            }
        }

        layers_.tail()->back_propagation(delta, idx);
    }

    float_t calc_delta(const vec_t* in, const vec_t* v, int data_size, vec_t& w, vec_t& dw, int check_index) {
        static const float_t delta = 1e-10;

        std::fill(dw.begin(), dw.end(), float_t(0));

        // calculate dw/dE by numeric
        float_t prev_w = w[check_index];

        w[check_index] = prev_w + delta;
        float_t f_p = float_t(0);
        for_i(data_size, [&](int i){ f_p += get_loss(fprop(in[i]), v[i]); });

        float_t f_m = float_t(0);
        w[check_index] = prev_w - delta;
        for_i(data_size, [&](int i){ f_m += get_loss(fprop(in[i]), v[i]); });

        float_t delta_by_numerical = (f_p - f_m) / (float_t(2) * delta);
        w[check_index] = prev_w;

        // calculate dw/dE by bprop
        for_i(data_size, [&](int i){ bprop(fprop(in[i]), v[i]); });

        float_t delta_by_bprop = dw[check_index];

        return std::abs(delta_by_bprop - delta_by_numerical);
    }

    void check_t(size_t i, label_t t, layer_size_t dim_out) {
        if (t >= dim_out) {
            std::ostringstream os;
            os << format_str("t[%u]=%u, dim(network output)=%u", i, t, dim_out) << std::endl;
            os << "in classification task, dim(network output) must be greater than max class id." << std::endl;
            if (dim_out == 1)
                os << std::endl << "(for regression, use vector<vec_t> instead of vector<label_t> for training signal)" << std::endl;

            throw nn_error("output dimension mismatch!\n " + os.str());
        }
    }

    void check_t(size_t i, const vec_t& t, layer_size_t dim_out) {
        if (t.size() != dim_out)
            throw nn_error(format_str("output dimension mismatch!\n dim(target[%u])=%u, dim(network output size=%u", i, t.size(), dim_out));
    }

    template <typename T>
    void check_training_data(const std::vector<vec_t>& in, const std::vector<T>& t) {
        layer_size_t dim_in = in_dim();
        layer_size_t dim_out = out_dim();

        if (in.size() != t.size())
            throw nn_error("number of training data must be equal to label data");

        size_t num = in.size();

        for (size_t i = 0; i < num; i++) {
            if (in[i].size() != dim_in)
                throw nn_error(format_str("input dimension mismatch!\n dim(data[%u])=%d, dim(network input)=%u", i, in[i].size(), dim_in));

            check_t(i, t[i], dim_out);
        }
    }

    float_t target_value_min() const { return layers_.tail()->activation_function().scale().first; }
    float_t target_value_max() const { return layers_.tail()->activation_function().scale().second; }

    std::string name_;
    Optimizer optimizer_;
    layers layers_;
};

/**
 * @brief [cut an image in samples to be tested (slow)]
 * @details [long description]
 * 
 * @param data [pointer to the data]
 * @param rows [self explained]
 * @param cols [self explained]
 * @param sizepatch [size of the patch, such as the total number of pixel in the patch is sizepatch*sizepatch ]
 * @return [vector of vec_c (sample) to be passed to test function]
 */
inline std::vector<vec_t> image2vec(const float_t* data, const unsigned int  rows, const unsigned int cols, const unsigned int sizepatch, const unsigned int step=1)
{
    assert(step>0);
    std::vector<vec_t> res((cols-sizepatch)*(rows-sizepatch)/(step*step),vec_t(sizepatch*sizepatch));
        for_i((cols-sizepatch)*(rows-sizepatch)/(step*step), [&](int count)
        {
            const int j = step*(count / ((cols-sizepatch)/step));
            const int i = step*(count % ((cols-sizepatch)/step));

            //vec_t sample(sizepatch*sizepatch);

            if (i+sizepatch < cols && j+sizepatch < rows)
            for (unsigned int k=0;k<sizepatch*sizepatch;k++)
            //for_i(sizepatch*sizepatch, [&](int k)
            {
                unsigned int y = k / sizepatch + j;
                unsigned int x = k % sizepatch + i;
                res[count][k] = data[x+y*cols];
            }
            //);
            //res[count] = (sample);
        });


    return res;
}

template <typename L, typename O, typename Layer>
network<L, O>& operator << (network<L, O>& n, const Layer&& l) {
    n.add(std::make_shared<Layer>(l));
    return n;
}

template <typename L, typename O, typename Layer>
network<L, O>& operator << (network<L, O>& n, Layer& l) {
    n.add(std::make_shared<Layer>(l));
    return n;
}

template <typename L, typename O, typename Char, typename CharTraits>
std::basic_ostream<Char, CharTraits>& operator << (std::basic_ostream<Char, CharTraits>& os, const network<L, O>& n) {
    n.save(os);
    return os;
}

template <typename L, typename O, typename Char, typename CharTraits>
std::basic_istream<Char, CharTraits>& operator >> (std::basic_istream<Char, CharTraits>& os, network<L, O>& n) {
    n.load(os);
    return os;
}

} // namespace tiny_cnn
