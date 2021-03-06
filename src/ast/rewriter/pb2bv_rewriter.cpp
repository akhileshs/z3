/*++
Copyright (c) 2016 Microsoft Corporation

Module Name:

    pb2bv_rewriter.cpp

Abstract:

    Conversion from pseudo-booleans to bit-vectors.

Author:

    Nikolaj Bjorner (nbjorner) 2016-10-23

Notes:

--*/

#include"rewriter.h"
#include"rewriter_def.h"
#include"statistics.h"
#include"pb2bv_rewriter.h"
#include"sorting_network.h"
#include"ast_util.h"
#include"ast_pp.h"
#include"lbool.h"


struct pb2bv_rewriter::imp {

    struct argc_t {
        expr*    m_arg;
        rational m_coeff;
        argc_t():m_arg(0), m_coeff(0) {}
        argc_t(expr* arg, rational const& r): m_arg(arg), m_coeff(r) {}
    };
    
    struct argc_gt {
        bool operator()(argc_t const& a, argc_t const& b) const {
            return a.m_coeff > b.m_coeff;
        }
    };

    struct argc_entry {
        unsigned m_index;
        rational m_k;
        expr*    m_value;
        argc_entry(unsigned i, rational const& k): m_index(i), m_k(k), m_value(0) {}
        argc_entry():m_index(0), m_k(0), m_value(0) {}
        
        struct eq {
            bool operator()(argc_entry const& a, argc_entry const& b) const {
                return a.m_index == b.m_index && a.m_k == b.m_k;
            }
        };
        struct hash {
            unsigned operator()(argc_entry const& a) const {
                return a.m_index ^ a.m_k.hash();
            }
        };
    };
    typedef hashtable<argc_entry, argc_entry::hash, argc_entry::eq> argc_cache;

    ast_manager&              m;
    params_ref                m_params;
    expr_ref_vector           m_lemmas;
    func_decl_ref_vector      m_fresh;       // all fresh variables
    unsigned_vector           m_fresh_lim;
    unsigned                  m_num_translated;

    struct card2bv_rewriter {               
        typedef expr* literal;
        typedef ptr_vector<expr> literal_vector;
        psort_nw<card2bv_rewriter> m_sort;
        ast_manager& m;
        imp&         m_imp;
        arith_util   au;
        pb_util      pb;
        bv_util      bv;
        expr_ref_vector m_trail;

        template<lbool is_le>
        expr_ref mk_le_ge(expr_ref_vector& fmls, expr* a, expr* b, expr* bound) {
            expr_ref x(m), y(m), result(m);
            unsigned nb = bv.get_bv_size(a);
            x = bv.mk_zero_extend(1, a);
            y = bv.mk_zero_extend(1, b);
            result = bv.mk_bv_add(x, y);
            x = bv.mk_extract(nb, nb, result);
            result = bv.mk_extract(nb-1, 0, result);
            if (is_le != l_false) {
                fmls.push_back(m.mk_eq(x, bv.mk_numeral(rational::zero(), 1)));
                fmls.push_back(bv.mk_ule(result, bound));
            }
            else {
                fmls.push_back(m.mk_eq(x, bv.mk_numeral(rational::one(), 1)));
                fmls.push_back(bv.mk_ule(bound, result));
            }
            return result;

        }

        // 
        // create a circuit of size sz*log(k) 
        // by forming a binary tree adding pairs of values that are assumed <= k, 
        // and in each step we check that the result is <= k by checking the overflow
        // bit and that the non-overflow bits are <= k.
        // The procedure for checking >= k is symmetric and checking for = k is
        // achieved by checking <= k on intermediary addends and the resulting sum is = k.
        // 
        template<lbool is_le>
        expr_ref mk_le_ge(func_decl *f, unsigned sz, expr * const* args, rational const & k) {
            if (k.is_zero()) {
                if (is_le != l_false) {
                    return expr_ref(m.mk_not(mk_or(m, sz, args)), m);
                }
                else {
                    return expr_ref(m.mk_true(), m);
                }
            }
            SASSERT(k.is_pos());
            expr_ref zero(m), bound(m);
            expr_ref_vector es(m), fmls(m);
            unsigned nb = k.get_num_bits();            
            zero = bv.mk_numeral(rational(0), nb);
            bound = bv.mk_numeral(k, nb);
            for (unsigned i = 0; i < sz; ++i) {
                if (pb.get_coeff(f, i) > k) {
                    if (is_le != l_false) {
                        fmls.push_back(m.mk_not(args[i]));
                    }
                    else {
                        fmls.push_back(args[i]);
                    }
                }
                else {
                    es.push_back(mk_ite(args[i], bv.mk_numeral(pb.get_coeff(f, i), nb), zero));
                }
            }
            while (es.size() > 1) {
                for (unsigned i = 0; i + 1 < es.size(); i += 2) {
                    es[i/2] = mk_le_ge<is_le>(fmls, es[i].get(), es[i+1].get(), bound);
                }
                if ((es.size() % 2) == 1) {
                    es[es.size()/2] = es.back();
                }
                es.shrink((1 + es.size())/2);
            }
            switch (is_le) {
            case l_true: 
                return mk_and(fmls);
            case l_false:
                fmls.push_back(bv.mk_ule(bound, es.back()));
                return mk_or(fmls);
            case l_undef:
                fmls.push_back(m.mk_eq(bound, es.back()));
                return mk_and(fmls);
            default:
                UNREACHABLE();
                return expr_ref(m.mk_true(), m);
            }
        }

        expr_ref mk_bv(func_decl * f, unsigned sz, expr * const* args) {
            decl_kind kind = f->get_decl_kind();
            rational k = pb.get_k(f);
            SASSERT(!k.is_neg());
            switch (kind) {
            case OP_PB_GE:
            case OP_AT_LEAST_K: {
                expr_ref_vector nargs(m);
                nargs.append(sz, args);
                dualize(f, nargs, k);
                SASSERT(!k.is_neg());
                return mk_le_ge<l_true>(f, sz, nargs.c_ptr(), k);
            }
            case OP_PB_LE:
            case OP_AT_MOST_K:
                return mk_le_ge<l_true>(f, sz, args, k);
            case OP_PB_EQ:
                return mk_le_ge<l_undef>(f, sz, args, k);
            default:
                UNREACHABLE();
                return expr_ref(m.mk_true(), m);
            }
        }

        void dualize(func_decl* f, expr_ref_vector & args, rational & k) {
            k.neg();
            for (unsigned i = 0; i < args.size(); ++i) {
                k += pb.get_coeff(f, i);
                args[i] = ::mk_not(m, args[i].get());
            }            
        }

        expr* negate(expr* e) {
            if (m.is_not(e, e)) return e;
            return m.mk_not(e);
        }
        expr* mk_ite(expr* c, expr* hi, expr* lo) {
            while (m.is_not(c, c)) {
                std::swap(hi, lo);
            }
            if (hi == lo) return hi;
            if (m.is_true(hi) && m.is_false(lo)) return c;
            if (m.is_false(hi) && m.is_true(lo)) return negate(c);
            if (m.is_true(hi)) return m.mk_or(c, lo);
            if (m.is_false(lo)) return m.mk_and(c, hi);
            if (m.is_false(hi)) return m.mk_and(negate(c), lo);
            if (m.is_true(lo)) return m.mk_implies(c, hi);
            return m.mk_ite(c, hi, lo);
        }
        
        bool is_or(func_decl* f) {
            switch (f->get_decl_kind()) {
            case OP_AT_MOST_K:
            case OP_PB_LE:
                return false;
            case OP_AT_LEAST_K:
            case OP_PB_GE: 
                return pb.get_k(f).is_one();
            case OP_PB_EQ:
                return false;
            default:
                UNREACHABLE();
                return false;
            }
        }


    public:

        card2bv_rewriter(imp& i, ast_manager& m):
            m(m),
            m_imp(i),
            au(m),
            pb(m),
            bv(m),
            m_sort(*this),
            m_trail(m)
        {}

        br_status mk_app_core(func_decl * f, unsigned sz, expr * const* args, expr_ref & result) {
            if (f->get_family_id() == pb.get_family_id()) {
                mk_pb(f, sz, args, result);
                ++m_imp.m_num_translated;
                return BR_DONE;
            }
            else if (f->get_family_id() == au.get_family_id() && mk_arith(f, sz, args, result)) {
                ++m_imp.m_num_translated;
                return BR_DONE;
            }
            else {
                return BR_FAILED;
            }
        }

        //
        // NSB: review
        // we should remove this code and rely on a layer above to deal with 
        // whatever it accomplishes. It seems to break types.
        // 
        bool mk_arith(func_decl * f, unsigned sz, expr * const* args, expr_ref & result) {
            if (f->get_decl_kind() == OP_ADD) {
                unsigned bits = 0;
                for (unsigned i = 0; i < sz; i++) {
                    rational val1, val2;
                    if (au.is_int(args[i]) && au.is_numeral(args[i], val1)) {
                        bits += val1.get_num_bits();
                    }
                    else if (m.is_ite(args[i]) &&
                             au.is_numeral(to_app(args[i])->get_arg(1), val1) && val1.is_one() &&
                             au.is_numeral(to_app(args[i])->get_arg(2), val2) && val2.is_zero()) {
                        bits++;                        
                    }
                    else
                        return false;
                }
                
                result = 0;
                for (unsigned i = 0; i < sz; i++) {
                    rational val1, val2;
                    expr * q;
                    if (au.is_int(args[i]) && au.is_numeral(args[i], val1))
                        q = bv.mk_numeral(val1, bits);
                    else
                        q = mk_ite(to_app(args[i])->get_arg(0), bv.mk_numeral(1, bits), bv.mk_numeral(0, bits));
                    result = (i == 0) ? q : bv.mk_bv_add(result.get(), q);
                }                
                return true;
            }
            else {
                return false;
            } 
        }

        void mk_pb(func_decl * f, unsigned sz, expr * const* args, expr_ref & result) {
            SASSERT(f->get_family_id() == pb.get_family_id());
            if (is_or(f)) {
                result = m.mk_or(sz, args);
            }
            else if (pb.is_at_most_k(f) && pb.get_k(f).is_unsigned()) {
                result = m_sort.le(true, pb.get_k(f).get_unsigned(), sz, args);
            }
            else if (pb.is_at_least_k(f) && pb.get_k(f).is_unsigned()) {
                result = m_sort.ge(true, pb.get_k(f).get_unsigned(), sz, args);
            }
            else if (pb.is_eq(f) && pb.get_k(f).is_unsigned() && pb.has_unit_coefficients(f)) {
                result = m_sort.eq(pb.get_k(f).get_unsigned(), sz, args);
            }
            else if (pb.is_le(f) && pb.get_k(f).is_unsigned() && pb.has_unit_coefficients(f)) {
                result = m_sort.le(true, pb.get_k(f).get_unsigned(), sz, args);
            }
            else if (pb.is_ge(f) && pb.get_k(f).is_unsigned() && pb.has_unit_coefficients(f)) {
                result = m_sort.ge(true, pb.get_k(f).get_unsigned(), sz, args);
            }
            else {
                result = mk_bv(f, sz, args);
            }
        }
   
        // definitions used for sorting network
        literal mk_false() { return m.mk_false(); }
        literal mk_true() { return m.mk_true(); }
        literal mk_max(literal a, literal b) { return trail(m.mk_or(a, b)); }
        literal mk_min(literal a, literal b) { return trail(m.mk_and(a, b)); }
        literal mk_not(literal a) { if (m.is_not(a,a)) return a; return trail(m.mk_not(a)); }

        std::ostream& pp(std::ostream& out, literal lit) {  return out << mk_ismt2_pp(lit, m);  }
        
        literal trail(literal l) {
            m_trail.push_back(l);
            return l;
        }
        literal fresh() {
            expr_ref fr(m.mk_fresh_const("sn", m.mk_bool_sort()), m);
            m_imp.m_fresh.push_back(to_app(fr)->get_decl());
            return trail(fr);
        }
        
        void mk_clause(unsigned n, literal const* lits) {
            m_imp.m_lemmas.push_back(mk_or(m, n, lits));
        }        
    };

    struct card2bv_rewriter_cfg : public default_rewriter_cfg {
        card2bv_rewriter m_r;
        bool rewrite_patterns() const { return false; }
        bool flat_assoc(func_decl * f) const { return false; }
        br_status reduce_app(func_decl * f, unsigned num, expr * const * args, expr_ref & result, proof_ref & result_pr) {
            result_pr = 0;
            return m_r.mk_app_core(f, num, args, result);
        }
        card2bv_rewriter_cfg(imp& i, ast_manager & m):m_r(i, m) {}
    };
    
    class card_pb_rewriter : public rewriter_tpl<card2bv_rewriter_cfg> {
    public:
        card2bv_rewriter_cfg m_cfg;
        card_pb_rewriter(imp& i, ast_manager & m):
            rewriter_tpl<card2bv_rewriter_cfg>(m, false, m_cfg),
            m_cfg(i, m) {}
    };

    card_pb_rewriter m_rw;
    
    imp(ast_manager& m, params_ref const& p): 
        m(m), m_params(p), m_lemmas(m),
        m_fresh(m),
        m_num_translated(0), 
        m_rw(*this, m) {
    }

    void updt_params(params_ref const & p) {}
    unsigned get_num_steps() const { return m_rw.get_num_steps(); }
    void cleanup() { m_rw.cleanup(); }
    void operator()(expr * e, expr_ref & result, proof_ref & result_proof) {
        m_rw(e, result, result_proof);
    }
    void push() {
        m_fresh_lim.push_back(m_fresh.size());
    }
    void pop(unsigned num_scopes) {
        SASSERT(m_lemmas.empty()); // lemmas must be flushed before pop.
        if (num_scopes > 0) {
            SASSERT(num_scopes <= m_fresh_lim.size());
            unsigned new_sz = m_fresh_lim.size() - num_scopes;
            unsigned lim = m_fresh_lim[new_sz];
            m_fresh.resize(lim);
            m_fresh_lim.resize(new_sz);
        }
        m_rw.reset();
    }

    void flush_side_constraints(expr_ref_vector& side_constraints) { 
        side_constraints.append(m_lemmas);  
        m_lemmas.reset(); 
    }

    void collect_statistics(statistics & st) const {
        st.update("pb-aux-variables", m_fresh.size());
        st.update("pb-aux-clauses", m_rw.m_cfg.m_r.m_sort.m_stats.m_num_compiled_clauses);
    }

};


pb2bv_rewriter::pb2bv_rewriter(ast_manager & m, params_ref const& p) {  m_imp = alloc(imp, m, p); }
pb2bv_rewriter::~pb2bv_rewriter() { dealloc(m_imp); }
void pb2bv_rewriter::updt_params(params_ref const & p) { m_imp->updt_params(p); }
ast_manager & pb2bv_rewriter::m() const { return m_imp->m; }
unsigned pb2bv_rewriter::get_num_steps() const { return m_imp->get_num_steps(); }
void pb2bv_rewriter::cleanup() { ast_manager& mgr = m(); params_ref p = m_imp->m_params; dealloc(m_imp); m_imp = alloc(imp, mgr, p);  }
func_decl_ref_vector const& pb2bv_rewriter::fresh_constants() const { return m_imp->m_fresh; }
void pb2bv_rewriter::operator()(expr * e, expr_ref & result, proof_ref & result_proof) { (*m_imp)(e, result, result_proof); }
void pb2bv_rewriter::push() { m_imp->push(); }
void pb2bv_rewriter::pop(unsigned num_scopes) { m_imp->pop(num_scopes); }
void pb2bv_rewriter::flush_side_constraints(expr_ref_vector& side_constraints) { m_imp->flush_side_constraints(side_constraints); } 
unsigned pb2bv_rewriter::num_translated() const { return m_imp->m_num_translated; }

void pb2bv_rewriter::collect_statistics(statistics & st) const { m_imp->collect_statistics(st); }
