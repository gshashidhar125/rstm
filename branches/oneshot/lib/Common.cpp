#include "metadata.hpp"
#include "UndoLog.hpp"

namespace stm
{
  /**
   *  Array of all threads
   */
  TX* threads[MAX_THREADS];

  /**
   *  Thread-local pointer to self
   */
  __thread TX* Self = NULL;

  /*** Count of all threads ***/
  pad_word_t threadcount = {0};

  NOINLINE
  void UndoLog::undo()
  {
      for (iterator i = end() - 1, e = begin(); i >= e; --i)
          i->undo();
  }

}
