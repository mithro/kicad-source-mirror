/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2013 CERN
 * @author Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Copyright (C) 2016 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#ifndef __COROUTINE_H
#define __COROUTINE_H

#include <cstdlib>

#include <boost/version.hpp>
#include <type_traits>

#if BOOST_VERSION <= 106000
#include <boost/context/fcontext.hpp>
#else
#include <boost/context/execution_context.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>
#endif

/**
 *  Class COROUNTINE.
 *  Implements a coroutine. Wikipedia has a good explanation:
 *
 *  "Coroutines are computer program components that generalize subroutines to
 *  allow multiple entry points for suspending and resuming execution at certain locations.
 *  Coroutines are well-suited for implementing more familiar program components such as cooperative
 *  tasks, exceptions, event loop, iterators, infinite lists and pipes."
 *
 *  In other words, a coroutine can be considered a lightweight thread - which can be
 *  preempted only when it deliberately yields the control to the caller. This way,
 *  we avoid concurrency problems such as locking / race conditions.
 *
 *  Uses boost::context library to do the actual context switching.
 *
 *  This particular version takes a DELEGATE as an entry point, so it can invoke
 *  methods within a given object as separate coroutines.
 *
 *  See coroutine_example.cpp for sample code.
 */

template <typename ReturnType, typename ArgType>
class COROUTINE
{
public:
    COROUTINE() :
        COROUTINE( nullptr )
    {
    }

    /**
     * Constructor
     * Creates a coroutine from a member method of an object
     */
    template <class T>
    COROUTINE( T* object, ReturnType(T::* ptr)( ArgType ) ) :
        COROUTINE( std::bind( ptr, object, std::placeholders::_1 ) )
    {
    }

    /**
     * Constructor
     * Creates a coroutine from a delegate object
     */
    COROUTINE( std::function<ReturnType(ArgType)> aEntry ) :
        m_func( std::move( aEntry ) ),
        m_running( false ),
#if BOOST_VERSION <= 106000
        m_stack( nullptr ),
        m_stackSize( c_defaultStackSize ),
#endif
        m_caller( nullptr ),
        m_callee( nullptr )
    {
        // Avoid not initialized members, and make static analysers quiet
        m_args = 0;
        m_retVal = 0;
    }

    ~COROUTINE()
    {
#if BOOST_VERSION >= 105600
        delete m_callee;
#endif

#if BOOST_VERSION <= 106000
        delete m_caller;

        if( m_stack )
            free( m_stack );
#endif
    }

private:
#if BOOST_VERSION <= 106000
    using context_type = boost::context::fcontext_t;
#else
    using context_type = boost::context::execution_context<COROUTINE*>;
#endif

public:
    /**
     * Function Yield()
     *
     * Stops execution of the coroutine and returns control to the caller.
     * After a yield, Call() or Resume() methods invoked by the caller will
     * immediately return true, indicating that we are not done yet, just asleep.
     */
    void Yield()
    {
#if BOOST_VERSION <= 106000
        jump( m_callee, m_caller, false );
#else
        auto result = (*m_caller)( this );
        *m_caller = std::move( std::get<0>( result ) );
#endif
    }

    /**
     * Function Yield()
     *
     * Yield with a value - passes a value of given type to the caller.
     * Useful for implementing generator objects.
     */
    void Yield( ReturnType& aRetVal )
    {
        m_retVal = aRetVal;
#if BOOST_VERSION <= 106000
        jump( m_callee, m_caller, false );
#else
        m_caller( this );
#endif
    }

    /**
     * Function SetEntry()
     *
     * Defines the entry point for the coroutine, if not set in the constructor.
     */
    void SetEntry( std::function<ReturnType(ArgType)> aEntry )
    {
        m_func = std::move( aEntry );
    }

    /* Function Call()
     *
     * Starts execution of a coroutine, passing args as its arguments.
     * @return true, if the coroutine has yielded and false if it has finished its
     * execution (returned).
     */
    bool Call( ArgType aArgs )
    {
        assert( m_callee == NULL );
        assert( m_caller == NULL );

#if BOOST_VERSION <= 106000
        // fixme: Clean up stack stuff. Add a guard
        m_stack = malloc( c_defaultStackSize );

        // align to 16 bytes
        void* sp = (void*) ( ( ( (ptrdiff_t) m_stack ) + m_stackSize - 0xf ) & ( ~0x0f ) );

        // correct the stack size
        m_stackSize -= ( (size_t) m_stack + m_stackSize - (size_t) sp );
#endif

        m_args = &aArgs;

#if BOOST_VERSION < 105600
        m_callee = boost::context::make_fcontext( sp, m_stackSize, callerStub );
#elif BOOST_VERSION <= 106000
        m_callee = new context_type( boost::context::make_fcontext( sp, m_stackSize, callerStub ) );
#else
        m_callee = new context_type( std::allocator_arg_t(),
                    boost::context::protected_fixedsize_stack( c_defaultStackSize ), &COROUTINE::callerStub );
#endif

#if BOOST_VERSION <= 106000
        m_caller = new context_type();
#endif

        m_running = true;

        // off we go!
#if BOOST_VERSION <= 106000
        jump( m_caller, m_callee, reinterpret_cast<intptr_t>( this ) );
#else
        auto result = (*m_callee)( this );
        *m_callee = std::move( std::get<0>( result ) );
#endif
        return m_running;
    }

    /**
     * Function Resume()
     *
     * Resumes execution of a previously yielded coroutine.
     * @return true, if the coroutine has yielded again and false if it has finished its
     * execution (returned).
     */
    bool Resume()
    {
#if BOOST_VERSION <= 106000
        jump( m_caller, m_callee, false );
#else
        auto result = (*m_callee)( this );
        *m_callee = std::move( std::get<0>( result ) );
#endif

        return m_running;
    }

    /**
     * Function ReturnValue()
     *
     * Returns the yielded value (the argument Yield() was called with)
     */
    const ReturnType& ReturnValue() const
    {
        return m_retVal;
    }

    /**
     * Function Running()
     *
     * @return true, if the coroutine is active
     */
    bool Running() const
    {
        return m_running;
    }

private:
    static const int c_defaultStackSize = 2000000;    // fixme: make configurable

    /* real entry point of the coroutine */
#if BOOST_VERSION <= 106000
    static void callerStub( intptr_t aData )
#else
    static context_type callerStub( context_type caller, COROUTINE* cor )
#endif
    {
        // get pointer to self
#if BOOST_VERSION <= 106000
        COROUTINE<ReturnType, ArgType>* cor = reinterpret_cast<COROUTINE<ReturnType, ArgType>*>( aData );
#else
        cor->m_caller = &caller;
#endif

        // call the coroutine method
        cor->m_retVal = cor->m_func( *( cor->m_args ) );
        cor->m_running = false;

        // go back to wherever we came from.
#if BOOST_VERSION <= 106000
        jump( cor->m_callee, cor->m_caller, 0 );
#else
        return caller;
#endif
    }

    ///> Wrapper for jump_fcontext to assure compatibility between different boost versions
#if BOOST_VERSION <= 106000
    static inline intptr_t jump( context_type* aOld, context_type* aNew,
                                intptr_t aP, bool aPreserveFPU = true )
    {
#if BOOST_VERSION < 105600
        return boost::context::jump_fcontext( aOld, aNew, aP, aPreserveFPU );
#else
        return boost::context::jump_fcontext( aOld, *aNew, aP, aPreserveFPU );
#endif
    }
#endif

    std::function<ReturnType(ArgType)> m_func;

    bool m_running;

#if BOOST_VERSION <= 106000
    ///< coroutine stack
    void* m_stack;

    size_t m_stackSize;
#endif

    ///< pointer to coroutine entry arguments. Stripped of references
    ///< to avoid compiler errors.
    typename std::remove_reference<ArgType>::type* m_args;

    ReturnType m_retVal;

    ///< saved caller context
    context_type* m_caller;

    ///< saved coroutine context
    context_type* m_callee;
};

#endif
