/* (host tests: nothing of libogc) */
typedef unsigned int lwp_t;
static inline void LWP_SetThreadPriority(lwp_t thread, unsigned int prio)
{
	(void) thread;
	(void) prio;
}
